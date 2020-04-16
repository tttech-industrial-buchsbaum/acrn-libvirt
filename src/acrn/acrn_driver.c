#include <config.h>
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <uuid/uuid.h>
#include <libxml/xpathInternals.h>
#include "configmake.h"
#include "datatypes.h"
#include "node_device_conf.h"
#include "virdomainobjlist.h"
#include "virerror.h"
#include "viralloc.h"
#include "cpu/cpu.h"
#include "virhostcpu.h"
#include "vircommand.h"
#include "virthread.h"
#include "virstring.h"
#include "virfile.h"
#include "virhostdev.h"
#include "virnodesuspend.h"
#include "virnetdevbridge.h"
#include "virnetdevtap.h"
#include "virfdstream.h"
#include "virlog.h"
#include "domain_event.h"
#include "acrn_driver.h"
#include "acrn_domain.h"

#define VIR_FROM_THIS VIR_FROM_ACRN
#define ACRN_DM_PATH            "/usr/bin/acrn-dm"
#define ACRN_CTL_PATH           "/usr/bin/acrnctl"
#define ACRN_NAMESPACE_HREF     "http://libvirt.org/schemas/domain/acrn/0.0"
#define ACRN_OFFLINE_PATH       "/sys/class/vhm/acrn_vhm/offline_cpu"
#define SYSFS_CPU_PATH          "/sys/devices/system/cpu"
#define ACRN_AUTOSTART_DIR      SYSCONFDIR "/libvirt/acrn/autostart"
#define ACRN_CONFIG_DIR         SYSCONFDIR "/libvirt/acrn"
#define ACRN_NET_GENERATED_TAP_PREFIX   "tap"

VIR_LOG_INIT("acrn.acrn_driver");

typedef struct _acrnConnect acrnConnect;
typedef struct _acrnConnect *acrnConnectPtr;
struct _acrnConnect {
    virMutex lock;
    virNodeInfo nodeInfo;
    virDomainObjListPtr domains;
    virCapsPtr caps;
    virDomainXMLOptionPtr xmlopt;
    virObjectEventStatePtr domainEventState;
    virHostdevManagerPtr hostdevMgr;
};

typedef struct _acrnDomainNamespaceDef acrnDomainNamespaceDef;
typedef acrnDomainNamespaceDef *acrnDomainNamespaceDefPtr;
struct _acrnDomainNamespaceDef {
    size_t num_args;
    char **args;
};

static acrnConnectPtr acrn_driver = NULL;

static void
acrnDriverLock(acrnConnectPtr driver)
{
    virMutexLock(&driver->lock);
}

static void
acrnDriverUnlock(acrnConnectPtr driver)
{
    virMutexUnlock(&driver->lock);
}

/**
 * Get a reference to the virCapsPtr instance for the
 * driver.
 *
 * The caller must release the reference with virObjetUnref
 *
 * Returns: a reference to a virCapsPtr instance or NULL
 */
static virCapsPtr ATTRIBUTE_NONNULL(1)
acrnDriverGetCapabilities(acrnConnectPtr driver)
{
    return virObjectRef(driver->caps);
}

static virDomainObjPtr
acrnDomObjFromDomain(virDomainPtr domain)
{
    virDomainObjPtr vm;
    acrnConnectPtr privconn = domain->conn->privateData;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    vm = virDomainObjListFindByUUID(privconn->domains, domain->uuid);

    if (!vm) {
        virUUIDFormat(domain->uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s' (%s)"),
                       uuidstr, domain->name);
        return NULL;
    }

    return vm;
}

struct acrnFindUUIDData {
    unsigned char *uuid;
};

static int
acrnFindHvUUID(virDomainObjPtr vm, void *opaque)
{
    struct acrnFindUUIDData *data = opaque;
    acrnDomainObjPrivatePtr priv;
    int ret = 0;

    virObjectLock(vm);

    priv = vm->privateData;

    if (!uuid_compare(priv->hvUUID, data->uuid))
        ret = -1;

    virObjectUnlock(vm);
    return ret;
}

static int
acrnInitUUID(virDomainObjListPtr doms, unsigned char *uuid)
{
    static const char *const availUUIDStr[] = {
        "d2795438-25d6-11e8-864e-cb7a18b34643",
        "a7ada506-1ab0-4b6b-a0da-e513ca9b8c2f",
    };
    unsigned char availUUID[VIR_UUID_BUFLEN];
    struct acrnFindUUIDData data = { .uuid = availUUID };
    size_t i;

    for (i = 0; i < ARRAY_CARDINALITY(availUUIDStr); i++) {
        if (virUUIDParse(availUUIDStr[i], availUUID) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("virUUIDParse failed"));
            break;
        }

        if (!virDomainObjListForEach(doms, acrnFindHvUUID, &data)) {
            uuid_copy(uuid, availUUID);
            return 0;
        }
    }

    return -1;
}

static int
acrnCreateTapDev(virDomainNetDefPtr net, const unsigned char *uuid)
{
    int tapfd = -1, ret = -1;

    if (!net->ifname ||
        !STRPREFIX(net->ifname, ACRN_NET_GENERATED_TAP_PREFIX)) {
        if (net->ifname) {
            VIR_WARN("Tap name '%s' not supported", net->ifname);
            VIR_FREE(net->ifname);
        }
        if (VIR_STRDUP(net->ifname,
                       ACRN_NET_GENERATED_TAP_PREFIX "%d") < 0) {
            virReportError(VIR_ERR_NO_MEMORY, NULL);
            goto cleanup;
        }
    }

    if (virNetDevTapCreateInBridgePort(
                virDomainNetGetActualBridgeName(net),
                &net->ifname, &net->mac,
                uuid, NULL, &tapfd, 1,
                virDomainNetGetActualVirtPortProfile(net),
                virDomainNetGetActualVlan(net),
                NULL, 0, NULL,
                VIR_NETDEV_TAP_CREATE_IFUP |
                VIR_NETDEV_TAP_CREATE_PERSIST) < 0) {
        virReportError(VIR_WAR_NO_NETWORK, "%s", net->ifname);
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (tapfd >= 0)
        VIR_FORCE_CLOSE(tapfd);
    return ret;
}

static void
acrnNetCleanup(virDomainObjPtr vm)
{
    size_t i;

    for (i = 0; i < vm->def->nnets; i++) {
        virDomainNetDefPtr net = vm->def->nets[i];
        virDomainNetType actualType = virDomainNetGetActualType(net);

        if (actualType == VIR_DOMAIN_NET_TYPE_BRIDGE) {
            if (net->ifname) {
                int retries = 5;

                ignore_value(virNetDevBridgeRemovePort(
                                virDomainNetGetActualBridgeName(net),
                                net->ifname));

                /*
                 * FIXME
                 * There is currently no way to reliably know when the
                 * shutdown process is complete.
                 */
                while (virNetDevTapDelete(net->ifname, NULL) < 0 && retries--)
                    sleep(1);
            }
        }
    }
}

static int
acrnCreateTty(virDomainObjPtr vm, virDomainChrDefPtr chr)
{
    acrnDomainObjPrivatePtr priv = vm->privateData;
    int ttyfd;
    char *ttypath;

    if (priv->nttys == ARRAY_CARDINALITY(priv->ttys)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("too many ttys (max = %lu)"),
                       ARRAY_CARDINALITY(priv->ttys));
        return -1;
    }

    if (virFileOpenTty(&ttyfd, &ttypath, 0) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("virFileOpenTty failed"));
        return -1;
    }

    if (VIR_STRDUP(priv->ttys[priv->nttys].slave, ttypath) < 0) {
        virReportError(VIR_ERR_NO_MEMORY, NULL);
        VIR_FORCE_CLOSE(ttyfd);
        return -1;
    }
    priv->ttys[priv->nttys].fd = ttyfd;
    priv->nttys++;

    if (chr->source->data.file.path)
        VIR_FREE(chr->source->data.file.path);
    chr->source->data.file.path = ttypath;

    return 0;
}

static void
acrnTtyCleanup(virDomainObjPtr vm)
{
    acrnDomainObjPrivatePtr priv = vm->privateData;

    acrnDomainTtyCleanup(priv);
}

static void
acrnAddVirtioConsoleCmd(virBufferPtr buf, virDomainChrDefPtr chr)
{
    if (chr->deviceType != VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE ||
        chr->targetType != VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO)
        return;

    switch (chr->source->type) {
    case VIR_DOMAIN_CHR_TYPE_PTY:
        virBufferAddLit(buf, ",@pty:pty_port");
        break;
    case VIR_DOMAIN_CHR_TYPE_DEV:
        virBufferAsprintf(buf, ",@tty:tty_port=%s",
                          chr->source->data.file.path);
        break;
    case VIR_DOMAIN_CHR_TYPE_FILE:
        virBufferAsprintf(buf, ",@file:file_port=%s",
                          chr->source->data.file.path);
        break;
    case VIR_DOMAIN_CHR_TYPE_STDIO:
        virBufferAddLit(buf, ",@stdio:stdio_port");
        break;
    case VIR_DOMAIN_CHR_TYPE_UNIX:
        virBufferAsprintf(buf, ",socket:socket_file_name=%s:%s",
                          chr->source->data.nix.path,
                          chr->source->data.nix.listen ?
                          "server" : "client");
        break;
    default:
        return;
    }
}

struct acrnCmdDeviceData {
    virDomainObjPtr vm;
    virCommandPtr cmd;
    bool lpc;
};

static int
acrnCommandAddDeviceArg(virDomainDefPtr def,
                        virDomainDeviceDefPtr dev,
                        virDomainDeviceInfoPtr info,
                        void *opaque)
{
    struct acrnCmdDeviceData *data = opaque;
    virCommandPtr cmd = data->cmd;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK: {
        virDomainDiskDefPtr disk = dev->data.disk;

        if (disk->bus == VIR_DOMAIN_DISK_BUS_VIRTIO) {
            /*
             * VIR_DOMAIN_DISK_DEVICE_DISK &&
             * VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI
             */
            virCommandAddArg(cmd, "-s");
            virCommandAddArgFormat(cmd, "%u:%u:%u,virtio-blk,%s",
                                   info->addr.pci.bus,
                                   info->addr.pci.slot,
                                   info->addr.pci.function,
                                   virDomainDiskGetSource(disk));
        } else { /* VIR_DOMAIN_DISK_BUS_SATA */
            size_t i;

            for (i = 0; i < def->ncontrollers; i++) {
                virDomainControllerDefPtr ctrl = def->controllers[i];

                if (ctrl->type == VIR_DOMAIN_CONTROLLER_TYPE_SATA &&
                    ctrl->idx == disk->info.addr.drive.controller) {
                    virCommandAddArg(cmd, "-s");
                    virCommandAddArgFormat(cmd, "%u:%u:%u,ahci-%s,%s",
                                           ctrl->info.addr.pci.bus,
                                           ctrl->info.addr.pci.slot,
                                           ctrl->info.addr.pci.function,
                                           (disk->device ==
                                                VIR_DOMAIN_DISK_DEVICE_DISK) ?
                                                "hd" : "cd",
                                           virDomainDiskGetSource(disk));
                    /* a SATA controller can only have one disk attached */
                    break;
                }
            }
        }
        break;
    }
    case VIR_DOMAIN_DEVICE_NET: {
        virDomainNetDefPtr net = dev->data.net;
        char macstr[VIR_MAC_STRING_BUFLEN];

        if (net->type == VIR_DOMAIN_NET_TYPE_BRIDGE &&
            acrnCreateTapDev(net, def->uuid) < 0)
                return -1;

        virCommandAddArg(cmd, "-s");
        virCommandAddArgFormat(cmd, "%u:%u:%u,virtio-net,%s,mac=%s",
                               info->addr.pci.bus,
                               info->addr.pci.slot,
                               info->addr.pci.function,
                               net->ifname,
                               virMacAddrFormat(&net->mac, macstr));
        break;
    }
    case VIR_DOMAIN_DEVICE_HOSTDEV: {
        virDomainHostdevDefPtr hostdev = dev->data.hostdev;
        virDomainHostdevSubsysPtr subsys = &hostdev->source.subsys;

        virCommandAddArg(cmd, "-s");

        if (subsys->type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB) {
            virDomainHostdevSubsysUSBPtr usbsrc = &subsys->u.usb;

            if (!usbsrc->autoAddress) {
                virReportError(VIR_ERR_NO_SOURCE, _("usb hostdev"));
                return -1;
            }

            virCommandAddArgFormat(cmd, "%u:%u:%u,passthru,%x/%x/0",
                                   info->addr.pci.bus,
                                   info->addr.pci.slot,
                                   info->addr.pci.function,
                                   usbsrc->bus, usbsrc->device);
        } else { /* VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI */
            virDomainHostdevSubsysPCIPtr pcisrc = &subsys->u.pci;

            virCommandAddArgFormat(cmd, "%u:%u:%u,passthru,%x/%x/%x",
                                   info->addr.pci.bus,
                                   info->addr.pci.slot,
                                   info->addr.pci.function,
                                   pcisrc->addr.bus,
                                   pcisrc->addr.slot,
                                   pcisrc->addr.function);
        }
        break;
    }
    case VIR_DOMAIN_DEVICE_CONTROLLER: {
        virDomainControllerDefPtr ctrl = dev->data.controller;
        size_t i;
        bool found = false;
        virBuffer buf = VIR_BUFFER_INITIALIZER;

        /* PCI hostbridge is always included */
        if (ctrl->type == VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL) {
            for (i = 0; i < def->nconsoles; i++) {
                virDomainChrDefPtr chr = def->consoles[i];

                if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
                    chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL &&
                    chr->info.addr.vioserial.controller == ctrl->idx) {
                    if (!found) {
                        virBufferAsprintf(&buf, "%u:%u:%u,virtio-console",
                                          info->addr.pci.bus,
                                          info->addr.pci.slot,
                                          info->addr.pci.function);
                        found = true;
                    }

                    acrnAddVirtioConsoleCmd(&buf, chr);
                }
            }
        }

        if (found) {
            virCommandAddArg(cmd, "-s");
            virCommandAddArgBuffer(cmd, &buf);
            virBufferFreeAndReset(&buf);
        }
        break;
    }
    case VIR_DOMAIN_DEVICE_CHR: {
        virDomainChrDefPtr chr = dev->data.chr;

        if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL) {
            virBuffer buf = VIR_BUFFER_INITIALIZER;

            if (!data->lpc) {
                virCommandAddArgList(cmd, "-s", "1:0,lpc", NULL);
                data->lpc = true;
            }

            virBufferAsprintf(&buf, "com%d,", chr->target.port + 1);

            switch (chr->source->type) {
            case VIR_DOMAIN_CHR_TYPE_PTY:
                if (acrnCreateTty(data->vm, chr) < 0)
                    return -1;
                virBufferAsprintf(&buf, "%s", chr->source->data.file.path);
                break;
            case VIR_DOMAIN_CHR_TYPE_DEV:
                virBufferAsprintf(&buf, "%s", chr->source->data.file.path);
                break;
            case VIR_DOMAIN_CHR_TYPE_STDIO:
                virBufferAddLit(&buf, "stdio");
                break;
            case VIR_DOMAIN_CHR_TYPE_TCP: {
                unsigned int tcpport;

                if (virStrToLong_ui(chr->source->data.tcp.service,
                                    NULL, 10, &tcpport) < 0) {
                    virBufferFreeAndReset(&buf);
                    virReportError(VIR_ERR_NO_SOURCE,
                                   _("serial over tcp"));
                    return -1;
                }

                virBufferAsprintf(&buf, "tcp:%u", tcpport);
                break;
            }
            default:
                virBufferFreeAndReset(&buf);
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("serial type %s"),
                               virDomainChrTypeToString(chr->source->type));
                return -1;
            }

            virCommandAddArg(cmd, "-l");
            virCommandAddArgBuffer(cmd, &buf);
            virBufferFreeAndReset(&buf);
        } else { /* VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE */
            /* may be an implicit serial device - ignore */
            if (chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE ||
                chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL) {
                VIR_DEBUG("ignore implicit serial device");
                break;
            }

            /* VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO */
            if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
                virBuffer buf = VIR_BUFFER_INITIALIZER;

                virBufferAsprintf(&buf, "%u:%u:%u,virtio-console",
                                  info->addr.pci.bus,
                                  info->addr.pci.slot,
                                  info->addr.pci.function);
                acrnAddVirtioConsoleCmd(&buf, chr);

                virCommandAddArg(cmd, "-s");
                virCommandAddArgBuffer(cmd, &buf);
                virBufferFreeAndReset(&buf);
            }
            /*
             * VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL was
             * already dealt with when its controller was reached.
             */
        }
        break;
    }
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_RNG:
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("device type %s"),
                       virDomainDeviceTypeToString(dev->type));
        return -1;
    }

    return 0;
}

static virCommandPtr
acrnBuildStartCmd(virDomainObjPtr vm)
{
    virDomainDefPtr def;
    virCommandPtr cmd;
    acrnDomainObjPrivatePtr priv;
    struct acrnCmdDeviceData data = { 0 };
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (!vm || !(def = vm->def))
        return NULL;

    priv = vm->privateData;

    if (!(cmd = virCommandNew(ACRN_DM_PATH))) {
        virReportError(VIR_ERR_NO_MEMORY, NULL);
        return NULL;
    }

    /* ACPI */
    if (def->features[VIR_DOMAIN_FEATURE_ACPI] == VIR_TRISTATE_SWITCH_ON)
        virCommandAddArg(cmd, "-A");

    /* Memory */
    virCommandAddArg(cmd, "-m");
    virCommandAddArgFormat(cmd, "%lluM",
                           VIR_DIV_UP(virDomainDefGetMemoryInitial(def), 1024));

    /* UUID */
    if (virUUIDIsValid(priv->hvUUID)) {
        virCommandAddArg(cmd, "-U");
        virCommandAddArg(cmd, virUUIDFormat(priv->hvUUID, uuidstr));
    }

    /* PCI hostbridge */
    virCommandAddArgList(cmd, "-s", "0:0,hostbridge", NULL);

    data.vm = vm;
    data.cmd = cmd;

    /* Devices */
    if (virDomainDeviceInfoIterate(def, acrnCommandAddDeviceArg, &data)) {
        virCommandFree(cmd);
        return NULL;
    }

    /* Bootloader */
    if (def->os.loader && def->os.loader->path) {
        virBuffer buf = VIR_BUFFER_INITIALIZER;

        if (def->os.loader->readonly == VIR_TRISTATE_BOOL_NO)
            virBufferAddLit(&buf, "w,");
        virBufferAdd(&buf, def->os.loader->path, -1);

        virCommandAddArg(cmd, "--ovmf");
        virCommandAddArgBuffer(cmd, &buf);
        virBufferFreeAndReset(&buf);
    } else if (def->os.kernel && def->os.cmdline) {
        virCommandAddArg(cmd, "-k");
        virCommandAddArg(cmd, def->os.kernel);
        virCommandAddArg(cmd, "-B");
        virCommandAddArg(cmd, def->os.cmdline);

        if (def->os.initrd) {
            virCommandAddArg(cmd, "-r");
            virCommandAddArg(cmd, def->os.initrd);
        }
    } else {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("boot policy"));
    }

    /* VM name */
    virCommandAddArg(cmd, def->name);

    return cmd;
}

static int
acrnProcessStart(virDomainObjPtr vm)
{
    virCommandPtr cmd;
    int ret = -1;

    if (!(cmd = acrnBuildStartCmd(vm)))
        goto cleanup;

    virCommandDaemonize(cmd);

    VIR_DEBUG("Starting domain '%s'", vm->def->name);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    /* XXX */
    if (sscanf(vm->def->name, "vm%d", &vm->def->id) != 1 &&
        sscanf(vm->def->name, "instance-%d", &vm->def->id) != 1)
        vm->def->id = 0;

    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_BOOTED);
    ret = 0;

cleanup:
    virCommandFree(cmd);
    if (ret < 0) {
        acrnNetCleanup(vm);
        acrnTtyCleanup(vm);
    }
    return ret;
}

static virCommandPtr
acrnBuildStopCmd(virDomainDefPtr def)
{
    virCommandPtr cmd;

    if (!def)
        return NULL;

    if (!(cmd = virCommandNewArgList(ACRN_CTL_PATH, "stop", "-f",
                                     def->name, NULL)))
        virReportError(VIR_ERR_NO_MEMORY, NULL);

    return cmd;
}

static int
acrnProcessStop(virDomainObjPtr vm, int reason)
{
    virCommandPtr cmd;
    int ret = -1;

    if (!(cmd = acrnBuildStopCmd(vm->def)))
        goto cleanup;

    VIR_DEBUG("Stopping domain '%s'", vm->def->name);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    /* clean up network interfaces */
    acrnNetCleanup(vm);

    /* clean up ttys */
    acrnTtyCleanup(vm);

    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);

    vm->def->id = -1;
    ret = 0;

cleanup:
    virCommandFree(cmd);
    return ret;
}

static virDomainPtr
acrnDomainLookupByUUID(virConnectPtr conn,
                       const unsigned char *uuid)
{
    acrnConnectPtr privconn = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByUUID(privconn->domains, uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

cleanup:
    if (vm)
        virObjectUnlock(vm);
    return dom;
}

static virDomainPtr
acrnDomainLookupByName(virConnectPtr conn, const char *name)
{
    acrnConnectPtr privconn = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByName(privconn->domains, name);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), name);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static int
acrnDomainShutdown(virDomainPtr dom)
{
    acrnConnectPtr privconn = dom->conn->privateData;
    virDomainObjPtr vm;
    virObjectEventPtr event = NULL;
    int ret = -1;

    if (!(vm = acrnDomObjFromDomain(dom)))
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("domain is not running"));
        goto cleanup;
    }

    if (acrnProcessStop(vm, VIR_DOMAIN_SHUTOFF_SHUTDOWN) < 0)
        goto cleanup;

    if (!(event = virDomainEventLifecycleNewFromObj(
                    vm,
                    VIR_DOMAIN_EVENT_STOPPED,
                    VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN)))
        goto cleanup;

    ret = 0;

cleanup:
    if (vm)
        virObjectUnlock(vm);
    if (event)
        virObjectEventStateQueue(privconn->domainEventState, event);
    return ret;
}

static int
acrnDomainDestroy(virDomainPtr dom)
{
    acrnConnectPtr privconn = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainState state;
    virObjectEventPtr event = NULL;
    int reason, ret = -1;

    if (!(vm = acrnDomObjFromDomain(dom)))
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("domain is not running"));
        goto cleanup;
    }

    state = virDomainObjGetState(vm, &reason);

    if (state == VIR_DOMAIN_SHUTOFF) {
        if (reason != VIR_DOMAIN_SHUTOFF_DESTROYED)
            virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF,
                                 VIR_DOMAIN_SHUTOFF_DESTROYED);
    } else {
        if (acrnProcessStop(vm, VIR_DOMAIN_SHUTOFF_DESTROYED) < 0)
            goto cleanup;
    }

    event = virDomainEventLifecycleNewFromObj(
                vm,
                VIR_DOMAIN_EVENT_STOPPED,
                VIR_DOMAIN_EVENT_STOPPED_DESTROYED);

    if (!vm->persistent &&
        (state != VIR_DOMAIN_SHUTOFF ||
         reason != VIR_DOMAIN_SHUTOFF_DESTROYED)) {
        virDomainObjListRemove(privconn->domains, vm);
        vm = NULL;
    }

    if (!event)
        goto cleanup;

    ret = 0;

cleanup:
    if (vm)
        virObjectUnlock(vm);
    if (event)
        virObjectEventStateQueue(privconn->domainEventState, event);
    return ret;
}

static int
acrnDomainGetInfo(virDomainPtr dom, virDomainInfoPtr info)
{
    virDomainObjPtr vm;
    int ret = -1;

    if (!(vm = acrnDomObjFromDomain(dom)))
        goto cleanup;

    info->state = virDomainObjGetState(vm, NULL);
    info->maxMem = virDomainDefGetMemoryTotal(vm->def);
    info->nrVirtCpu = virDomainDefGetVcpus(vm->def);
    ret = 0;

cleanup:
    if (vm)
        virObjectUnlock(vm);
    return ret;
}

static int
acrnDomainGetState(virDomainPtr domain,
                   int *state,
                   int *reason,
                   unsigned int flags)
{
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = acrnDomObjFromDomain(domain)))
        goto cleanup;

    *state = virDomainObjGetState(vm, reason);
    ret = 0;

cleanup:
    if (vm)
        virObjectUnlock(vm);
    return ret;
}

static int
acrnDomainGetVcpus(virDomainPtr domain,
                   virVcpuInfoPtr info,
                   int maxinfo,
                   unsigned char *cpumaps,
                   int maplen)
{
    acrnConnectPtr privconn = domain->conn->privateData;
    virDomainObjPtr vm;
    virDomainDefPtr def;
    struct timeval tv;
    virBitmapPtr bitmap, allcpumap = NULL;
    unsigned long long statbase;
    int hostcpus, i, ret = -1;
    ssize_t pos;

    if (!(vm = acrnDomObjFromDomain(domain)))
        return -1;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("cannot list vcpus for an inactive domain"));
        goto cleanup;
    }

    if (!(def = vm->def))
        goto cleanup;

    if (gettimeofday(&tv, NULL) < 0) {
        virReportSystemError(errno,
                             "%s", _("getting time of day"));
        goto cleanup;
    }

    statbase = (tv.tv_sec * 1000UL * 1000UL) + tv.tv_usec;

    /* clamp to actual number of vcpus */
    if (maxinfo > virDomainDefGetVcpus(vm->def))
        maxinfo = virDomainDefGetVcpus(vm->def);

    memset(info, 0, sizeof(*info) * maxinfo);

    if (cpumaps)
        memset(cpumaps, 0, maxinfo * maplen);

    hostcpus = VIR_NODEINFO_MAXCPUS(privconn->nodeInfo);

    /*
     * FIXME
     *
     * There is no way of retrieving the real vCPU map.
     *
     * Blindly trust the cpumask provided, and fall back to
     * all available CPUs when it's absent.
     */
    if (def->cpumask) {
        bitmap = def->cpumask;
    } else {
        if (!(allcpumap = virBitmapNew(hostcpus))) {
            virReportError(VIR_ERR_NO_MEMORY, NULL);
            goto cleanup;
        }

        virBitmapSetAll(allcpumap);
        bitmap = allcpumap;
    }

    for (i = 0, pos = -1; i < maxinfo; i++) {
        virDomainVcpuDefPtr vcpu = virDomainDefGetVcpu(def, i);

        if (!vcpu->online)
            continue;

        if (cpumaps)
            virBitmapToDataBuf(bitmap, VIR_GET_CPUMAP(cpumaps, maplen, i),
                               maplen);

        info[i].number = i;
        info[i].state = VIR_VCPU_RUNNING;

        if ((pos = virBitmapNextSetBit(bitmap, pos)) < 0 || pos >= hostcpus)
            pos = virBitmapNextSetBit(bitmap, -1);
        if (pos >= hostcpus)
            pos = 0;
        info[i].cpu = pos;

        /* FIXME fake an increasing cpu time value */
        info[i].cpuTime = statbase / 10;
    }

    ret = maxinfo;

cleanup:
    if (vm)
        virObjectUnlock(vm);
    virBitmapFree(allcpumap);
    return ret;
}

static char *
acrnDomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    acrnConnectPtr privconn = domain->conn->privateData;
    virDomainObjPtr vm;
    virCapsPtr caps = NULL;
    char *ret = NULL;

    if (!(vm = acrnDomObjFromDomain(domain)))
        goto cleanup;

    if (!(caps = acrnDriverGetCapabilities(privconn)))
        goto cleanup;

    ret = virDomainDefFormat(vm->def, caps,
                             virDomainDefFormatConvertXMLFlags(flags));

    virObjectUnref(caps);

cleanup:
    if (vm)
        virObjectUnlock(vm);
    return ret;
}

static virDomainPtr
acrnDomainCreateXML(virConnectPtr conn,
                    const char *xml,
                    unsigned int flags)
{
    acrnConnectPtr privconn = conn->privateData;
    acrnDomainObjPrivatePtr priv;
    virCapsPtr caps = NULL;
    virDomainDefPtr def = NULL;
    virDomainObjPtr vm = NULL;
    virObjectEventPtr event = NULL;
    virDomainPtr dom = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;
    unsigned char hvUUID[VIR_UUID_BUFLEN];

    /* VIR_DOMAIN_START_AUTODESTROY is not supported yet */
    virCheckFlags(VIR_DOMAIN_START_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_START_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;

    if (!(caps = acrnDriverGetCapabilities(privconn)))
        return NULL;

    if (!(def = virDomainDefParseString(xml, caps, privconn->xmlopt,
                                        NULL, parse_flags)))
        goto cleanup;

    acrnDriverLock(privconn);

    /* get hv UUID for this vm */
    if (acrnInitUUID(privconn->domains, hvUUID) < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(privconn->domains, def,
                                   privconn->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                   VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE, NULL)))
        goto cleanup;

    priv = vm->privateData;
    uuid_copy(priv->hvUUID, hvUUID);

    def = NULL;

    if (acrnProcessStart(vm) < 0)
        goto cleanup;

    if (!(event = virDomainEventLifecycleNewFromObj(
                    vm,
                    VIR_DOMAIN_EVENT_STARTED,
                    VIR_DOMAIN_EVENT_STARTED_BOOTED))) {
        acrnProcessStop(vm, VIR_DOMAIN_SHUTOFF_DESTROYED);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

cleanup:
    if (vm) {
        if (!dom && !vm->persistent)
            /* if domain is not persistent, remove its data */
            virDomainObjListRemove(privconn->domains, vm);
        else
            virObjectUnlock(vm);
    }
    acrnDriverUnlock(privconn);
    virDomainDefFree(def);
    virObjectUnref(caps);
    if (event)
        virObjectEventStateQueue(privconn->domainEventState, event);
    return dom;
}

static int
acrnDomainCreateWithFlags(virDomainPtr domain, unsigned int flags)
{
    acrnConnectPtr privconn = domain->conn->privateData;
    virDomainObjPtr vm;
    virObjectEventPtr event = NULL;
    int ret = -1;

    /* VIR_DOMAIN_START_AUTODESTROY is not supported yet */
    virCheckFlags(0, -1);

    if (!(vm = acrnDomObjFromDomain(domain)))
        goto cleanup;

    if (virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("domain is already running"));
        goto cleanup;
    }

    if (acrnProcessStart(vm) < 0)
        /* domain must be persistent */
        goto cleanup;

    if (!(event = virDomainEventLifecycleNewFromObj(
                    vm,
                    VIR_DOMAIN_EVENT_STARTED,
                    VIR_DOMAIN_EVENT_STARTED_BOOTED))) {
        /* domain must be persistent */
        acrnProcessStop(vm, VIR_DOMAIN_SHUTOFF_DESTROYED);
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (vm)
        virObjectUnlock(vm);
    if (event)
        virObjectEventStateQueue(privconn->domainEventState, event);
    return ret;
}

static int
acrnDomainCreate(virDomainPtr domain)
{
    return acrnDomainCreateWithFlags(domain, 0);
}

static virDomainPtr
acrnDomainDefineXMLFlags(virConnectPtr conn, const char *xml,
                         unsigned int flags)
{
    acrnConnectPtr privconn = conn->privateData;
    acrnDomainObjPrivatePtr priv;
    virCapsPtr caps = NULL;
    virDomainDefPtr def = NULL, oldDef = NULL;
    virDomainObjPtr vm = NULL;
    virObjectEventPtr event = NULL;
    virDomainPtr dom = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;
    unsigned char hvUUID[VIR_UUID_BUFLEN];

    virCheckFlags(VIR_DOMAIN_DEFINE_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_DEFINE_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;

    if (!(caps = acrnDriverGetCapabilities(privconn)))
        return NULL;

    if (!(def = virDomainDefParseString(xml, caps, privconn->xmlopt,
                                        NULL, parse_flags)))
        goto cleanup;

    if (virXMLCheckIllegalChars("name", def->name, "\n") < 0)
        goto cleanup;

    acrnDriverLock(privconn);

    /* get hv UUID for this vm */
    if (acrnInitUUID(privconn->domains, hvUUID) < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(privconn->domains, def,
                                   privconn->xmlopt,
                                   0, &oldDef)))
        goto cleanup;

    vm->persistent = 1;
    priv = vm->privateData;
    uuid_copy(priv->hvUUID, hvUUID);

    def = NULL;

    if (virDomainSaveConfig(ACRN_CONFIG_DIR, caps,
                            vm->newDef ? vm->newDef : vm->def) < 0)
        goto cleanup;

    if (!(event = virDomainEventLifecycleNewFromObj(
                    vm,
                    VIR_DOMAIN_EVENT_DEFINED,
                    !oldDef ?
                    VIR_DOMAIN_EVENT_DEFINED_ADDED :
                    VIR_DOMAIN_EVENT_DEFINED_UPDATED))) {
        virDomainDeleteConfig(ACRN_CONFIG_DIR,
                              ACRN_AUTOSTART_DIR,
                              vm);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

cleanup:
    if (vm) {
        if (!dom)
            virDomainObjListRemove(privconn->domains, vm);
        else
            virObjectUnlock(vm);
    }
    acrnDriverUnlock(privconn);
    virDomainDefFree(def);
    virDomainDefFree(oldDef);
    virObjectUnref(caps);
    if (event)
        virObjectEventStateQueue(privconn->domainEventState, event);
    return dom;
}

static virDomainPtr
acrnDomainDefineXML(virConnectPtr conn, const char *xml)
{
    return acrnDomainDefineXMLFlags(conn, xml, 0);
}

static int
acrnDomainUndefineFlags(virDomainPtr domain, unsigned int flags)
{
    acrnConnectPtr privconn = domain->conn->privateData;
    virObjectEventPtr event = NULL;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = acrnDomObjFromDomain(domain)))
        goto cleanup;

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("cannot undefine transient domain"));
        goto cleanup;
    }

    if (virDomainDeleteConfig(ACRN_CONFIG_DIR,
                              ACRN_AUTOSTART_DIR,
                              vm) < 0)
        goto cleanup;

    event = virDomainEventLifecycleNewFromObj(
                vm,
                VIR_DOMAIN_EVENT_UNDEFINED,
                VIR_DOMAIN_EVENT_UNDEFINED_REMOVED);

    if (virDomainObjIsActive(vm)) {
        vm->persistent = 0;
    } else {
        virDomainObjListRemove(privconn->domains, vm);
        vm = NULL;
    }

    if (!event)
        goto cleanup;

    ret = 0;

cleanup:
    if (vm)
        virObjectUnlock(vm);
    if (event)
        virObjectEventStateQueue(privconn->domainEventState, event);
    return ret;
}

static int
acrnDomainUndefine(virDomainPtr domain)
{
    return acrnDomainUndefineFlags(domain, 0);
}

static int
acrnDomainOpenConsole(virDomainPtr dom,
                      const char *dev_name,
                      virStreamPtr st,
                      unsigned int flags)
{
    virDomainObjPtr vm;
    virDomainChrDefPtr chr = NULL;
    acrnDomainObjPrivatePtr priv;
    size_t i;
    int dupfd, ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = acrnDomObjFromDomain(dom)))
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("domain is not running"));
        goto cleanup;
    }

    if (vm->def->nserials &&
        vm->def->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)
        chr = vm->def->serials[0];
    else if (vm->def->nconsoles)
        chr = vm->def->consoles[0];

    if (!chr) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot find character device %s"),
                       NULLSTR(dev_name));
        goto cleanup;
    }

    if (chr->source->type != VIR_DOMAIN_CHR_TYPE_PTY) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("character device %s is not using a PTY"),
                       dev_name ? dev_name : NULLSTR(chr->info.alias));
        goto cleanup;
    }

    priv = vm->privateData;

    for (i = 0; i < priv->nttys; i++) {
        if (priv->ttys[i].slave &&
            chr->source->data.file.path &&
            STREQ(priv->ttys[i].slave, chr->source->data.file.path))
            break;
    }

    if (i == priv->nttys) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot find character device %s"),
                       NULLSTR(dev_name));
        goto cleanup;
    }

    /* dup the master's fd so it can be closed by the caller */
    if ((dupfd = dup(priv->ttys[i].fd)) < 0) {
        virReportSystemError(errno, "%s", _("dup"));
        goto cleanup;
    }

    if (virFDStreamOpen(st, dupfd) < 0) {
        VIR_FORCE_CLOSE(dupfd);
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (vm)
        virObjectUnlock(vm);
    return ret;
}

static virDrvOpenStatus
acrnConnectOpen(virConnectPtr conn,
                virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                virConfPtr conf ATTRIBUTE_UNUSED,
                unsigned int flags)
{
     virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

     if (conn->uri == NULL) {
         if (acrn_driver == NULL)
             return VIR_DRV_OPEN_DECLINED;

         if (!(conn->uri = virURIParse("acrn:///system")))
             return VIR_DRV_OPEN_ERROR;
     } else {
         if (STRNEQ_NULLABLE(conn->uri->scheme, "acrn"))
             return VIR_DRV_OPEN_DECLINED;

         if (conn->uri->server)
             return VIR_DRV_OPEN_DECLINED;

         if (STRNEQ_NULLABLE(conn->uri->path, "/system")) {
             virReportError(VIR_ERR_INTERNAL_ERROR,
                            _("Unexpected ACRN URI path '%s', try acrn:///system"),
                            conn->uri->path ? conn->uri->path : "NULL");
             return VIR_DRV_OPEN_ERROR;
         }

         if (acrn_driver == NULL) {
             virReportError(VIR_ERR_INTERNAL_ERROR,
                            _("ACRN state driver is not active"));
             return VIR_DRV_OPEN_ERROR;
         }
     }

     conn->privateData = acrn_driver;

     return VIR_DRV_OPEN_SUCCESS;
}

static int
acrnConnectClose(virConnectPtr conn)
{
    /* autodestroy is not supported yet */
    conn->privateData = NULL;
    return 0;
}

static const char *
acrnConnectGetType(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return "ACRN";
}

static int
acrnConnectGetVersion(virConnectPtr conn ATTRIBUTE_UNUSED,
                      unsigned long *version)
{
    virCommandPtr cmd;
    char *verstr = NULL;
    const char *dmstr = "DM version is: ";
    int ret = -1;

    if (!(cmd = virCommandNewArgList(ACRN_DM_PATH, "-v", NULL))) {
        virReportError(VIR_ERR_NO_MEMORY, NULL);
        goto cleanup;
    }

    virCommandSetOutputBuffer(cmd, &verstr);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (!(dmstr = STRSKIP(verstr, dmstr)))
        goto cleanup;

    if (virParseVersionString(dmstr, version, true) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown release: %s"), dmstr);
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (verstr)
        VIR_FREE(verstr);
    virCommandFree(cmd);
    return ret;
}

static char *
acrnConnectGetHostname(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return virGetHostname();
}

static char *
acrnConnectGetCapabilities(virConnectPtr conn)
{
    acrnConnectPtr privconn = conn->privateData;
    virCapsPtr caps;
    char *xml;

    if (!(caps = acrnDriverGetCapabilities(privconn))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unable to get capabilities"));
        return NULL;
    }

    xml = virCapabilitiesFormatXML(caps);
    virObjectUnref(caps);
    return xml;
}

static int
acrnConnectListAllDomains(virConnectPtr conn,
                          virDomainPtr **domains,
                          unsigned int flags)
{
    acrnConnectPtr privconn = conn->privateData;

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);

    return virDomainObjListExport(privconn->domains, conn, domains, NULL,
                                  flags);
}

static char *
acrnConnectBaselineCPU(virConnectPtr conn ATTRIBUTE_UNUSED,
                       const char **xmlCPUs,
                       unsigned int ncpus,
                       unsigned int flags)
{
    virCPUDefPtr *cpus;
    virCPUDefPtr cpu = NULL;
    char *cpustr = NULL;

    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES |
                  VIR_CONNECT_BASELINE_CPU_MIGRATABLE, NULL);

    if (!(cpus = virCPUDefListParse(xmlCPUs, ncpus, VIR_CPU_TYPE_HOST)))
        goto cleanup;

    if (!(cpu = virCPUBaseline(VIR_ARCH_NONE, cpus, ncpus, NULL, NULL,
                            !!(flags & VIR_CONNECT_BASELINE_CPU_MIGRATABLE))))
        goto cleanup;

    if ((flags & VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES) &&
        virCPUExpandFeatures(cpus[0]->arch, cpu) < 0)
        goto cleanup;

    cpustr = virCPUDefFormat(cpu, NULL);

cleanup:
    virCPUDefFree(cpu);
    virCPUDefListFree(cpus);
    return cpustr;
}

static int
acrnConnectDomainEventRegisterAny(virConnectPtr conn,
                                  virDomainPtr dom,
                                  int eventID,
                                  virConnectDomainEventGenericCallback callback,
                                  void *opaque,
                                  virFreeCallback freecb)
{
    acrnConnectPtr privconn = conn->privateData;
    int ret;

    if (virDomainEventStateRegisterID(conn,
                                      privconn->domainEventState,
                                      dom, eventID,
                                      callback, opaque, freecb, &ret) < 0)
        ret = -1;

    return ret;
}

static int
acrnConnectDomainEventDeregisterAny(virConnectPtr conn,
                                    int callbackID)
{
    acrnConnectPtr privconn = conn->privateData;

    if (virObjectEventStateDeregisterID(conn,
                                        privconn->domainEventState,
                                        callbackID, true) < 0)
        return -1;

    return 0;
}

static int
acrnNodeGetInfo(virConnectPtr conn,
                virNodeInfoPtr info)
{
    acrnConnectPtr privconn = conn->privateData;

    memcpy(info, &privconn->nodeInfo, sizeof(*info));
    return 0;
}

static int
acrnNodeDeviceGetPCIInfo(virNodeDeviceDefPtr def,
                         unsigned *domain,
                         unsigned *bus,
                         unsigned *slot,
                         unsigned *function)
{
    virNodeDevCapsDefPtr cap = def->caps;

    while (cap) {
        if (cap->data.type == VIR_NODE_DEV_CAP_PCI_DEV) {
            *domain   = cap->data.pci_dev.domain;
            *bus      = cap->data.pci_dev.bus;
            *slot     = cap->data.pci_dev.slot;
            *function = cap->data.pci_dev.function;
            break;
        }

        cap = cap->next;
    }

    if (!cap) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("device %s is not a PCI device"), def->name);
        return -1;
    }

    return 0;
}

static int
acrnNodeDeviceDetachFlags(virNodeDevicePtr dev,
                          const char *driverName,
                          unsigned int flags)
{
    char *xml;
    virNodeDeviceDefPtr def = NULL;
    virPCIDevicePtr pci = NULL;
    acrnConnectPtr privconn = dev->conn->privateData;
    virHostdevManagerPtr hostdev_mgr = privconn->hostdevMgr;
    unsigned domain = 0, bus = 0, slot = 0, function = 0;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(xml = virNodeDeviceGetXMLDesc(dev, 0)))
        goto cleanup;

    if (!(def = virNodeDeviceDefParseString(xml, EXISTING_DEVICE, NULL)))
        goto cleanup;

    if (acrnNodeDeviceGetPCIInfo(def, &domain, &bus, &slot, &function) < 0)
        goto cleanup;

    if (!(pci = virPCIDeviceNew(domain, bus, slot, function)))
        goto cleanup;

    /* use the pci-stub driver */
    if (!driverName || STREQ(driverName, "kvm")) {
        virPCIDeviceSetStubDriver(pci, VIR_PCI_STUB_DRIVER_KVM);
    } else {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("unsupported driver name '%s'"), driverName);
        goto cleanup;
    }

    if (virHostdevPCINodeDeviceDetach(hostdev_mgr, pci) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    virPCIDeviceFree(pci);
    virNodeDeviceDefFree(def);
    if (xml)
        VIR_FREE(xml);
    return ret;
}

static int
acrnNodeDeviceDettach(virNodeDevicePtr dev)
{
    return acrnNodeDeviceDetachFlags(dev, NULL, 0);
}

static int
acrnNodeDeviceReAttach(virNodeDevicePtr dev)
{
    char *xml;
    virNodeDeviceDefPtr def = NULL;
    virPCIDevicePtr pci = NULL;
    acrnConnectPtr privconn = dev->conn->privateData;
    virHostdevManagerPtr hostdev_mgr = privconn->hostdevMgr;
    unsigned domain = 0, bus = 0, slot = 0, function = 0;
    int ret = -1;

    if (!(xml = virNodeDeviceGetXMLDesc(dev, 0)))
        goto cleanup;

    if (!(def = virNodeDeviceDefParseString(xml, EXISTING_DEVICE, NULL)))
        goto cleanup;

    if (acrnNodeDeviceGetPCIInfo(def, &domain, &bus, &slot, &function) < 0)
        goto cleanup;

    if (!(pci = virPCIDeviceNew(domain, bus, slot, function)))
        goto cleanup;

    if (virHostdevPCINodeDeviceReAttach(hostdev_mgr, pci) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    virPCIDeviceFree(pci);
    virNodeDeviceDefFree(def);
    if (xml)
        VIR_FREE(xml);
    return ret;
}

static int
acrnNodeGetCPUMap(virConnectPtr conn ATTRIBUTE_UNUSED,
                  unsigned char **cpumap,
                  unsigned int *online,
                  unsigned int flags)
{
    virBitmapPtr cpus = NULL;
    int ret = -1;
    int dummy;

    virCheckFlags(0, -1);

    if (!cpumap && !online)
        return virHostCPUGetCount();

    /*
     * XXX
     * We use virHostCPUGetPresentBitmap() here rather than
     * virHostCPUGetOnlineBitmap() because offline CPUs in
     * SOS are still available for vCPU allocation. This is,
     * however, counterintuitive.
     */
    if (!(cpus = virHostCPUGetPresentBitmap()))
        goto cleanup;

    if (cpumap && virBitmapToData(cpus, cpumap, &dummy) < 0)
        goto cleanup;

    ret = virBitmapCountBits(cpus);

    if (online)
        *online = ret;

cleanup:
    if (ret < 0 && cpumap && *cpumap)
        VIR_FREE(*cpumap);
    virBitmapFree(cpus);
    return ret;
}

static virCapsPtr
virAcrnCapsBuild(void)
{
    virCapsPtr caps;
    virCapsGuestPtr guest;

    if (!(caps = virCapabilitiesNew(virArchFromHost(), false, false)))
        return NULL;

    if (!(guest = virCapabilitiesAddGuest(caps,
                                          VIR_DOMAIN_OSTYPE_HVM,
                                          VIR_ARCH_X86_64, "acrn-dm",
                                          NULL, 0, NULL)))
        goto error;

    if (!virCapabilitiesAddGuestDomain(guest,
                                       VIR_DOMAIN_VIRT_ACRN,
                                       NULL, NULL, 0, NULL))
        goto error;

#if 0
    if (virCapabilitiesSetNetPrefix(caps, ACRN_NET_GENERATED_TAP_PREFIX) < 0) {
        virReportError(VIR_ERR_NO_MEMORY, NULL);
        goto error;
    }
#endif

    if (virCapabilitiesInitNUMA(caps) < 0) {
        virCapabilitiesFreeNUMAInfo(caps);
        VIR_WARN("Failed to query host NUMA topology, "
                 "disabling NUMA capabilities");
    }

    if (virCapabilitiesInitCaches(caps) < 0)
        VIR_WARN("Failed to get host CPU cache info");

    if (!(caps->host.cpu = virCPUProbeHost(caps->host.arch)))
        VIR_WARN("Failed to get host CPU");

    /* add the power management features of the host */
    if (virNodeSuspendGetTargetMask(&caps->host.powerMgmt) < 0)
        VIR_WARN("Failed to get host power management capabilities");

    /* add huge pages info */
    if (virCapabilitiesInitPages(caps) < 0)
        VIR_WARN("Failed to get pages info");

    return caps;

error:
    virObjectUnref(caps);
    return NULL;
}

/*
 * Vacate CPUs for UOS vCPU allocation.
 */
static int
acrnOfflineCpus(int nprocs)
{
    int i, fd;
    char path[128], chr, online;
    ssize_t rc;

    /* cpu0 can't be offlined */
    for (i = 1; i < nprocs; i++) {
        snprintf(path, sizeof(path), "%s/cpu%d/online", SYSFS_CPU_PATH, i);

        if ((fd = open(path, O_RDWR)) < 0) {
            virReportError(VIR_ERR_OPEN_FAILED, _("%s"), path);
            return -1;
        }

        chr = '0';

        do {
            if (pwrite(fd, &chr, sizeof(chr), 0) < sizeof(chr)) {
                close(fd);
                virReportError(VIR_ERR_WRITE_FAILED, _("%s"), path);
                return -1;
            }
        } while ((rc = pread(fd, &online, sizeof(online), 0)) > 0 &&
                 online != '0');

        close(fd);

        if (rc <= 0) {
            virReportError(VIR_ERR_READ_FAILED, _("%s"), path);
            return -1;
        }

        if ((fd = open(ACRN_OFFLINE_PATH, O_WRONLY)) < 0) {
            virReportError(VIR_ERR_OPEN_FAILED, _(ACRN_OFFLINE_PATH));
            return -1;
        }

        chr += i;

        if (write(fd, &chr, sizeof(chr)) < sizeof(chr)) {
            close(fd);
            virReportError(VIR_ERR_WRITE_FAILED, _(ACRN_OFFLINE_PATH));
            return -1;
        }

        close(fd);
    }

    return 0;
}

static int
acrnStateCleanup(void)
{
    VIR_DEBUG("ACRN state cleanup");

    if (!acrn_driver)
        return -1;

    virObjectUnref(acrn_driver->hostdevMgr);
    virObjectUnref(acrn_driver->domainEventState);
    virObjectUnref(acrn_driver->xmlopt);
    virObjectUnref(acrn_driver->caps);
    virObjectUnref(acrn_driver->domains);
    virMutexDestroy(&acrn_driver->lock);
    VIR_FREE(acrn_driver);

    return 0;
}

static int
acrnStateInitialize(bool privileged,
                    virStateInhibitCallback callback ATTRIBUTE_UNUSED,
                    void *opaque ATTRIBUTE_UNUSED)
{
    if (!privileged) {
        VIR_INFO("Not running privileged, disabling driver");
        return 0;
    }

    if (VIR_ALLOC(acrn_driver) < 0)
        return -1;

    if (virMutexInit(&acrn_driver->lock) < 0) {
        VIR_FREE(acrn_driver);
        return -1;
    }

    /* store a copy of node info before CPU offlining */
    if (virCapabilitiesGetNodeInfo(&acrn_driver->nodeInfo) < 0)
        goto cleanup;

    if (!(acrn_driver->domains = virDomainObjListNew()))
        goto cleanup;

    if (!(acrn_driver->caps = virAcrnCapsBuild()))
        goto cleanup;

    if (!(acrn_driver->xmlopt = virAcrnDriverCreateXMLConf()))
        goto cleanup;

    if (!(acrn_driver->domainEventState = virObjectEventStateNew()))
        goto cleanup;

    if (!(acrn_driver->hostdevMgr = virHostdevManagerGetDefault()))
        goto cleanup;

    if (virDomainObjListLoadAllConfigs(acrn_driver->domains,
                                       ACRN_CONFIG_DIR,
                                       ACRN_AUTOSTART_DIR, false,
                                       acrn_driver->caps,
                                       acrn_driver->xmlopt,
                                       NULL, NULL) < 0)
        goto cleanup;

    if (acrnOfflineCpus(get_nprocs_conf()) < 0)
        goto cleanup;

    return 0;

cleanup:
    acrnStateCleanup();
    return -1;
}

static virHypervisorDriver acrnHypervisorDriver = {
    .name = "ACRN",
    .connectOpen = acrnConnectOpen, /* 0.0.1 */
    .connectClose = acrnConnectClose, /* 0.0.1 */
    .connectGetType = acrnConnectGetType, /* 0.0.1 */
    .connectGetVersion = acrnConnectGetVersion, /* 0.0.1 */
    .connectGetHostname = acrnConnectGetHostname, /* 0.0.1 */
    .nodeGetInfo = acrnNodeGetInfo, /* 0.0.1 */
    .connectGetCapabilities = acrnConnectGetCapabilities, /* 0.0.1 */
    .connectListAllDomains = acrnConnectListAllDomains, /* 0.0.1 */
    .domainCreateXML = acrnDomainCreateXML, /* 0.0.1 */
    .domainLookupByUUID = acrnDomainLookupByUUID, /* 0.0.1 */
    .domainLookupByName = acrnDomainLookupByName, /* 0.0.1 */
    .domainShutdown = acrnDomainShutdown, /* 0.0.1 */
    .domainDestroy = acrnDomainDestroy, /* 0.0.1 */
    .domainGetInfo = acrnDomainGetInfo,  /* 0.0.1 */
    .domainGetState = acrnDomainGetState, /* 0.0.1 */
    .domainGetVcpus = acrnDomainGetVcpus, /* 0.0.1 */
    .domainGetXMLDesc = acrnDomainGetXMLDesc, /* 0.0.1 */
    .domainCreate = acrnDomainCreate, /* 0.0.1 */
    .domainCreateWithFlags = acrnDomainCreateWithFlags, /* 0.0.1 */
    .domainDefineXML = acrnDomainDefineXML, /* 0.0.1 */
    .domainDefineXMLFlags = acrnDomainDefineXMLFlags, /* 0.0.1 */
    .domainUndefine = acrnDomainUndefine, /* 0.0.1 */
    .domainUndefineFlags = acrnDomainUndefineFlags, /* 0.0.1 */
    .nodeDeviceDettach = acrnNodeDeviceDettach, /* 0.0.1 */
    .nodeDeviceDetachFlags = acrnNodeDeviceDetachFlags, /* 0.0.1 */
    .nodeDeviceReAttach = acrnNodeDeviceReAttach, /* 0.0.1 */
    .connectBaselineCPU = acrnConnectBaselineCPU, /* 0.0.1 */
    .connectDomainEventRegisterAny = acrnConnectDomainEventRegisterAny, /* 0.0.1 */
    .connectDomainEventDeregisterAny = acrnConnectDomainEventDeregisterAny, /* 0.0.1 */
    .domainOpenConsole = acrnDomainOpenConsole, /* 0.0.1 */
    .nodeGetCPUMap = acrnNodeGetCPUMap, /* 0.0.1 */
};

static virConnectDriver acrnConnectDriver = {
    .hypervisorDriver = &acrnHypervisorDriver,
};

static virStateDriver acrnStateDriver = {
    .name = "ACRN",
    .stateInitialize = acrnStateInitialize,
    .stateCleanup = acrnStateCleanup,
};

int
acrnRegister(void)
{
    if (virRegisterConnectDriver(&acrnConnectDriver, true) < 0)
        return -1;
    if (virRegisterStateDriver(&acrnStateDriver) < 0)
        return -1;
    return 0;
}
