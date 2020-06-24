#ifndef __ACRN_MONITOR__
#define __ACRN_MONITOR__

#include "vircommand.h"
#include "domain_conf.h"

typedef void (*acrnDomainMonitorStopCallback)(virDomainObjPtr vm);
typedef struct _acrnDomainMonitor acrnDomainMonitor;
typedef acrnDomainMonitor *acrnDomainMonitorPtr;

int acrnDomainMonitorStart(virDomainObjPtr domain,
                           acrnDomainMonitorStopCallback stop,
                           virCommandPtr cmd);
void acrnDomainMonitorStop(acrnDomainMonitorPtr mon);
int acrnDomainMonitorGetReason(acrnDomainMonitorPtr mon);

#endif /* __ACRN_MONITOR__ */
