/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Ioctl.h - default I/O queue: IOCTL_PFVHCI_* dispatch, everything else to UdeCx.
 */
#pragma once

#include "Common.h"

/* Creates the default (parallel, passive-level, not power-managed) queue of the controller. */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfIoctlCreateDefaultQueue(_Inout_ PF_CONTROLLER_CONTEXT* Ctrl);
