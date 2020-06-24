#include <config.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "viralloc.h"
#include "virthread.h"
#include "virlog.h"
#include "virfile.h"
#include "acrn_domain.h"
#include "acrn_monitor.h"

#define VIR_FROM_THIS VIR_FROM_ACRN
VIR_LOG_INIT("acrn.acrn_driver");

struct _acrnDomainMonitor {
    int pipefd[2];
    int watch;
    virThread thread;
    virCommandPtr cmd;
    virDomainObjPtr vm;
    acrnDomainMonitorStopCallback stop;
    int shutdown_reason;
    virMutex lock;
};

static void
acrnDomainMonitorLock(acrnDomainMonitorPtr mon)
{
	virMutexLock(&mon->lock);
}

static void
acrnDomainMonitorUnlock(acrnDomainMonitorPtr mon)
{
	virMutexUnlock(&mon->lock);
}

static void
acrnDomainMonitorEventHandleCB(int watch, int fd, int events, void *opaque)
{
    virDomainObjPtr domain = opaque;
    acrnDomainObjPrivatePtr priv;
    acrnDomainMonitorPtr mon;
    int reason = VIR_DOMAIN_SHUTOFF_UNKNOWN;

    if (!domain)
        return;

    priv = domain->privateData;
    if (!priv)
        return;

    mon = priv->mon;
    if (!mon)
        return;

    if (events & VIR_EVENT_HANDLE_READABLE) {
        int ret;
        int status = 0;

        ret = read(fd, &status, sizeof(status));
        if (ret < 0) {
            virReportSystemError(errno, "%s", _("Reading monitor pipe failed"));
        }

        if (WIFSIGNALED(status)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Guest %s terminated by signal %d"),
                           domain->def->name,
                           WTERMSIG(status));
            reason = VIR_DOMAIN_SHUTOFF_CRASHED;
        } else if (WIFEXITED(status)) {
            VIR_INFO("Guest %s shut itself down; destroying domain.",
                     domain->def->name);
            reason = VIR_DOMAIN_SHUTOFF_SHUTDOWN;
        } else {
            VIR_WARN("Guest %s shut down by unknown reason.",
                     domain->def->name);
            reason = VIR_DOMAIN_SHUTOFF_UNKNOWN;
        }
    }

    if (events & (VIR_EVENT_HANDLE_ERROR | VIR_EVENT_HANDLE_HANGUP)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Monitor thread terminated, guest %s shutdown."),
                           domain->def->name);
            reason = VIR_DOMAIN_SHUTOFF_UNKNOWN;
    }

    acrnDomainMonitorLock(mon);
    mon->shutdown_reason = reason;
    mon->watch = -1;
    acrnDomainMonitorUnlock(mon);

    ignore_value(virEventRemoveHandle(watch));
}

static void
acrnDomainMonitorFreeCB(void *opaque)
{
    virDomainObjPtr domain = opaque;
    acrnDomainObjPrivatePtr priv = domain->privateData;

    if (priv && priv->mon && priv->mon->stop)
        priv->mon->stop(domain);
}

static void
acrnDomainMonitorThread(void *opaque)
{
    int ret;
    int status = 0;
    acrnDomainMonitorPtr mon = opaque;

    ret = virCommandWait(mon->cmd, &status);

    acrnDomainMonitorLock(mon);

    virCommandFree(mon->cmd);
    mon->cmd = NULL;
    if (ret < 0) {
        VIR_FORCE_CLOSE(mon->pipefd[0]);
        VIR_FORCE_CLOSE(mon->pipefd[1]);
    }

    if (mon->pipefd[1] > 0) {
        ignore_value(write(mon->pipefd[1], &status, sizeof(status)));
    }

    acrnDomainMonitorUnlock(mon);
}

/* called with mon->lock  held */
static void
acrnDomainMonitorCleanup(acrnDomainMonitorPtr mon)
{
    virDomainObjPtr domain = mon->vm;

    if (domain && domain->privateData) {
        acrnDomainObjPrivatePtr priv = mon->vm->privateData;

        priv->mon = NULL;
    }

    /* do not call back on cleanup */
    mon->stop = NULL;
    if (mon->watch > 0) {
        virEventRemoveHandle(mon->watch);
        mon->watch = -1;
    }

    VIR_FORCE_CLOSE(mon->pipefd[0]);
    VIR_FORCE_CLOSE(mon->pipefd[1]);
}

int
acrnDomainMonitorStart(virDomainObjPtr domain,
                       acrnDomainMonitorStopCallback stop,
                       virCommandPtr cmd)
{
    int ret;
    acrnDomainMonitorPtr mon = NULL;
    acrnDomainObjPrivatePtr priv = domain->privateData;

    if (domain->pid < 0)
        return -1;

    if (VIR_ALLOC(mon) < 0)
        return -1;

    ret = virMutexInit(&mon->lock);
    if (ret < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize monitor mutex"));
        goto monfree;
    }
    mon->pipefd[0] = -1;
    mon->pipefd[1] = -1;
    mon->watch = -1;
    mon->shutdown_reason = VIR_DOMAIN_SHUTOFF_UNKNOWN;
    mon->stop = stop;
    mon->cmd = cmd;
    mon->vm = domain;

    acrnDomainMonitorLock(mon);
    priv->mon = mon;

    ret = pipe2(mon->pipefd, O_CLOEXEC);
    if (ret < 0) {
        virReportSystemError(errno, "%s", _("Could not create monitor pipe"));
        goto cleanup;
    }

    mon->watch = virEventAddHandle(mon->pipefd[0],
                                   VIR_EVENT_HANDLE_READABLE |
                                   VIR_EVENT_HANDLE_ERROR |
                                   VIR_EVENT_HANDLE_HANGUP,
                                   acrnDomainMonitorEventHandleCB,
                                   domain,
                                   acrnDomainMonitorFreeCB);
    if (mon->watch < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to register monitor events"));
        goto cleanup;
    }

    ret = virThreadCreate(&mon->thread, true, acrnDomainMonitorThread, mon);
    if (ret < 0) {
        virReportSystemError(errno, "%s", _("Could not create monitor thread"));
        goto cleanup;
    }

    acrnDomainMonitorUnlock(mon);
    return 0;

cleanup:
    acrnDomainMonitorCleanup(mon);
    acrnDomainMonitorUnlock(mon);
monfree:
    VIR_FREE(mon);
    return -1;
}

void
acrnDomainMonitorStop(acrnDomainMonitorPtr mon)
{
    acrnDomainMonitorLock(mon);
    acrnDomainMonitorCleanup(mon);
    acrnDomainMonitorUnlock(mon);

    virThreadJoin(&mon->thread);
    VIR_FREE(mon);
}

int
acrnDomainMonitorGetReason(acrnDomainMonitorPtr mon)
{
    return mon->shutdown_reason;
}

