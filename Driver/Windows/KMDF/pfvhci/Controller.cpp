/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Controller.cpp - WDFDEVICE of the virtual host controller: UdeCx registration,
 * device interfaces, file objects (ownership of plugged devices), PnP callbacks.
 */
#include "Controller.h"
#include "Ioctl.h"
#include "RecordQueue.h"
#include "UsbDevice.h"

namespace {

/* {5D1D9FCF-12B3-4DDF-BB2B-3B2E8EBB2584} */
const GUID PfvhciInterfaceGuid = PFVHCI_INTERFACE_GUID_INIT;

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

/*
 * HKLM\SYSTEM\CurrentControlSet\Services\pfvhci\Parameters
 *   NumberOfUsb20Ports (REG_DWORD, default 8, 1..127)
 *   NumberOfUsb30Ports (REG_DWORD, default 8, 1..127)
 * Read once at device start; changes need a controller restart (pfdevcon update or
 * disable/enable).
 */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfReadPortConfiguration(_Out_ ULONG* Usb2Ports, _Out_ ULONG* Usb3Ports)
{
    PAGED_CODE();

    *Usb2Ports = PF_DEFAULT_USB2_PORTS;
    *Usb3Ports = PF_DEFAULT_USB3_PORTS;

    WDFKEY key = nullptr;
    NTSTATUS status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (NT_SUCCESS(status)) {
        UNICODE_STRING usb2Name = RTL_CONSTANT_STRING(L"NumberOfUsb20Ports");
        UNICODE_STRING usb3Name = RTL_CONSTANT_STRING(L"NumberOfUsb30Ports");
        ULONG value = 0;
        if (NT_SUCCESS(WdfRegistryQueryULong(key, &usb2Name, &value))) {
            *Usb2Ports = value;
        }
        if (NT_SUCCESS(WdfRegistryQueryULong(key, &usb3Name, &value))) {
            *Usb3Ports = value;
        }
        WdfRegistryClose(key);
    } else {
        PF_TRACE("no Parameters key (0x%08X), using default port counts\n", status);
    }

    /* UdeCx needs at least one port of each kind; the port table holds PF_MAX_PORTS. */
    if (*Usb2Ports < 1) {
        *Usb2Ports = 1;
    }
    if (*Usb2Ports > PF_MAX_PORTS_PER_TYPE) {
        *Usb2Ports = PF_MAX_PORTS_PER_TYPE;
    }
    if (*Usb3Ports < 1) {
        *Usb3Ports = 1;
    }
    if (*Usb3Ports > PF_MAX_PORTS_PER_TYPE) {
        *Usb3Ports = PF_MAX_PORTS_PER_TYPE;
    }
}

/* ------------------------------------------------------------------------ */
/* UdeCx controller callbacks                                               */
/* ------------------------------------------------------------------------ */

/*
 * GUID_USB_CAPABILITY_SELECTIVE_SUSPEND is answered by UdeCx itself. Chained MDLs
 * and function suspend are not supported. The two "connection compatible" queries
 * are answered positively so that Windows does not suggest moving the device to a
 * faster port (the virtual ports always match the device speed).
 */
_Function_class_(EVT_UDECX_WDF_DEVICE_QUERY_USB_CAPABILITY)
NTSTATUS PfEvtQueryUsbCapability(_In_ WDFDEVICE UdecxWdfDevice, _In_ GUID* CapabilityType, _In_ ULONG OutputBufferLength,
                                 _Out_writes_to_opt_(OutputBufferLength, *ResultLength) PVOID OutputBuffer,
                                 _Out_ ULONG* ResultLength)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);

    *ResultLength = 0;
    if (*CapabilityType == GUID_USB_CAPABILITY_DEVICE_CONNECTION_HIGH_SPEED_COMPATIBLE ||
        *CapabilityType == GUID_USB_CAPABILITY_DEVICE_CONNECTION_SUPER_SPEED_COMPATIBLE) {
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

/*
 * Controller reset. With ResetAction = UdecxWdfDeviceResetActionResetEachUsbDevice
 * UdeCx resets the attached devices individually (EvtUsbDeviceReset), so there is
 * nothing to do for the (virtual) controller itself.
 * VERIFY: UdecxWdfDeviceResetComplete's parameter list (documented as the WDFDEVICE only).
 */
_Function_class_(EVT_UDECX_WDF_DEVICE_RESET)
void PfEvtControllerReset(_In_ WDFDEVICE UdecxWdfDevice)
{
    PF_INFO("controller reset requested\n");
    UdecxWdfDeviceResetComplete(UdecxWdfDevice);
}

/* ------------------------------------------------------------------------ */
/* PnP                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Disable / uninstall / upgrade of the controller. Devices are unplugged here
 * (owners receive DEVICE_REMOVED) so that the child stacks go away before the
 * controller. If PnP later cancels the query-remove, user mode simply plugs the
 * devices in again. Windows does not send query-remove on shutdown.
 */
_Function_class_(EVT_WDF_DEVICE_QUERY_REMOVE)
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfEvtDeviceQueryRemove(_In_ WDFDEVICE Device)
{
    PAGED_CODE();
    PF_INFO("controller query-remove: unplugging all devices\n");
    PfDeviceUnplugAll(PfGetControllerContext(Device));
    return STATUS_SUCCESS;
}

_Function_class_(EVT_WDF_OBJECT_CONTEXT_CLEANUP)
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfEvtControllerCleanup(_In_ WDFOBJECT Object)
{
    /*
     * Drop port-table references of devices that were never unplugged explicitly
     * (surprise removal of the controller). UdeCx/WDF delete the child objects; the
     * reference only kept their memory alive.
     */
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(Object);
    for (ULONG i = 0; i < PF_MAX_PORTS; ++i) {
        if (ctrl->Ports[i] != nullptr) {
            WdfObjectDereference(ctrl->Ports[i]);
            ctrl->Ports[i] = nullptr;
        }
    }
    PF_INFO("controller cleanup\n");
}

/* ------------------------------------------------------------------------ */
/* File objects                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Handles opened through the private interface (\PFVHCI reference string) are
 * "control" handles that may use IOCTL_PFVHCI_*. Handles opened through
 * GUID_DEVINTERFACE_USB_HOST_CONTROLLER (no file name) may only send the
 * standard host-controller IOCTLs. Anything else is refused.
 */
_Function_class_(EVT_WDF_DEVICE_FILE_CREATE)
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfEvtDeviceFileCreate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ WDFFILEOBJECT FileObject)
{
    PAGED_CODE();

    PF_FILE_CONTEXT* file = PfGetFileContext(FileObject);
    file->FileObject = FileObject;
    file->Controller = Device;
    InitializeListHead(&file->Records);
    InitializeListHead(&file->Devices);
    file->StandaloneCount = 0;
    file->Closing = FALSE;
    file->PumpActive = FALSE;
    file->IsControl = FALSE;

    NTSTATUS status = STATUS_SUCCESS;
    PCUNICODE_STRING name = WdfFileObjectGetFileName(FileObject);
    if (name != nullptr && name->Length != 0) {
        UNICODE_STRING tail = *name;
        while (tail.Length >= sizeof(WCHAR) && tail.Buffer[0] == L'\\') {
            ++tail.Buffer;
            tail.Length = static_cast<USHORT>(tail.Length - sizeof(WCHAR));
            tail.MaximumLength = static_cast<USHORT>(tail.MaximumLength - sizeof(WCHAR));
        }
        UNICODE_STRING expected = RTL_CONSTANT_STRING(PF_PRIVATE_REFERENCE_STRING);
        if (RtlEqualUnicodeString(&tail, &expected, TRUE)) {
            file->IsControl = TRUE;
        } else {
            status = STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    WdfRequestComplete(Request, status);
}

/*
 * Last handle closed (or the owning process died). Unplugs every device this
 * handle owns, failing their pending and in-flight URBs with
 * STATUS_DEVICE_NOT_CONNECTED / USBD_STATUS_DEVICE_GONE, then cancels pended FETCHes.
 */
_Function_class_(EVT_WDF_FILE_CLEANUP)
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfEvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
    PAGED_CODE();

    PF_FILE_CONTEXT* file = PfGetFileContext(FileObject);
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(WdfFileObjectGetDevice(FileObject));

    if (file->IsControl) {
        PF_INFO("control handle cleanup\n");
    }
    PfDeviceUnplugAllOwnedBy(ctrl, file);
    PfFileRecordsCleanup(ctrl, file);
}

/* Kept nonpaged: nothing to free here, everything was released in EvtFileCleanup. */
_Function_class_(EVT_WDF_FILE_CLOSE)
void PfEvtFileClose(_In_ WDFFILEOBJECT FileObject)
{
    PF_FILE_CONTEXT* file = PfGetFileContext(FileObject);
    NT_ASSERT(IsListEmpty(&file->Devices));
    NT_ASSERT(IsListEmpty(&file->Records));
    UNREFERENCED_PARAMETER(file);
}

} // namespace

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfControllerCreate(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceQueryRemove = PfEvtDeviceQueryRemove;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, PfEvtDeviceFileCreate, PfEvtFileClose, PfEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, PF_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);

    /*
     * Device object security. The INF applies the same SDDL to the device (PDO and
     * stack); this covers the named FDO. SYSTEM and Administrators only: the
     * ParsFlash client runs as a LocalSystem service. See Docs/DRIVER.md (Security).
     */
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_AUTOGENERATED_DEVICE_NAME, TRUE);
    NTSTATUS status = WdfDeviceInitAssignSDDLString(DeviceInit, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfDeviceInitAssignSDDLString 0x%08X\n", status);
        return status;
    }

    status = UdecxInitializeWdfDeviceInit(DeviceInit);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("UdecxInitializeWdfDeviceInit 0x%08X\n", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, PF_CONTROLLER_CONTEXT);
    deviceAttributes.EvtCleanupCallback = PfEvtControllerCleanup;

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfDeviceCreate 0x%08X\n", status);
        return status;
    }
    /* From here on, WDF deletes the device (and every child object) if we fail. */

    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(device);
    ctrl->Device = device;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctrl->Lock);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfSpinLockCreate 0x%08X\n", status);
        return status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &ctrl->PlugLock);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfWaitLockCreate 0x%08X\n", status);
        return status;
    }

    PfReadPortConfiguration(&ctrl->NumUsb2Ports, &ctrl->NumUsb3Ports);

    status = PfRecordQueueInitialize(ctrl);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Looks like any other host controller to USBView & co. */
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_USB_HOST_CONTROLLER, nullptr);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfDeviceCreateDeviceInterface(USB_HOST_CONTROLLER) 0x%08X\n", status);
        return status;
    }
    UNICODE_STRING reference = RTL_CONSTANT_STRING(PF_PRIVATE_REFERENCE_STRING);
    status = WdfDeviceCreateDeviceInterface(device, &PfvhciInterfaceGuid, &reference);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfDeviceCreateDeviceInterface(PFVHCI) 0x%08X\n", status);
        return status;
    }

    UDECX_WDF_DEVICE_CONFIG udecxConfig;
    UDECX_WDF_DEVICE_CONFIG_INIT(&udecxConfig, PfEvtQueryUsbCapability);
    udecxConfig.NumberOfUsb20Ports = static_cast<USHORT>(ctrl->NumUsb2Ports);
    udecxConfig.NumberOfUsb30Ports = static_cast<USHORT>(ctrl->NumUsb3Ports);
    udecxConfig.ResetAction = UdecxWdfDeviceResetActionResetEachUsbDevice;
    udecxConfig.EvtUdecxWdfDeviceReset = PfEvtControllerReset;

    status = UdecxWdfDeviceAddUsbDeviceEmulation(device, &udecxConfig);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("UdecxWdfDeviceAddUsbDeviceEmulation 0x%08X\n", status);
        return status;
    }

    status = PfIoctlCreateDefaultQueue(ctrl);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PF_INFO("controller ready: %u USB 2.0 + %u USB 3.0 ports, driver %s, ABI %u\n", ctrl->NumUsb2Ports,
            ctrl->NumUsb3Ports, PFVHCI_VERSION_STRING, PFVHCI_ABI_VERSION);
    return STATUS_SUCCESS;
}
