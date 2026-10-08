/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Controller.h - the emulated USB host controller (root-enumerated WDFDEVICE).
 */
#pragma once

#include "Common.h"

/* EvtDriverDeviceAdd body: configures DeviceInit for UdeCx and creates the controller. */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfControllerCreate(_Inout_ PWDFDEVICE_INIT DeviceInit);
