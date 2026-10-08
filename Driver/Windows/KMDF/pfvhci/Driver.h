/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Driver.h - driver object entry points.
 */
#pragma once

#include "Common.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfEvtDriverDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit);

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfEvtDriverContextCleanup(_In_ WDFOBJECT DriverObject);
