/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Driver.cpp - DriverEntry and EvtDriverDeviceAdd.
 *
 * pfvhci.sys is a root-enumerated (ROOT\ParsFlashVhci) KMDF driver that uses the
 * USB Device Emulation class extension (UdeCx, udecx.sys) to present a virtual
 * USB host controller. Devices attached to it are backed by a user-mode client
 * that talks to a remote ParsFlash server (see Docs/DRIVER.md).
 */
#include "Driver.h"
#include "Controller.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    PF_INFO("%s %s loading (ABI %u)\n", PFVHCI_FILE_DESCRIPTION, PFVHCI_VERSION_STRING, PFVHCI_ABI_VERSION);

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, PfEvtDriverDeviceAdd);
    config.DriverPoolTag = PF_POOL_TAG;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = PfEvtDriverContextCleanup;

    const NTSTATUS status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfDriverCreate 0x%08X\n", status);
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfEvtDriverDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);
    return PfControllerCreate(DeviceInit);
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfEvtDriverContextCleanup(_In_ WDFOBJECT DriverObject)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(DriverObject);
    PF_INFO("driver unloading\n");
}
