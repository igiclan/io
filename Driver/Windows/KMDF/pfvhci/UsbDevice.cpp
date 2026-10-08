/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * UsbDevice.cpp - IOCTL_PFVHCI_PLUGIN parsing, UDECXUSBDEVICE creation, plug-in,
 * unplug, port allocation and the UDECXUSBDEVICE state-change callbacks.
 *
 * Device lifetime:
 *   PLUGIN  -> UdecxUsbDeviceCreate -> published in the port table (+1 WDF reference,
 *              State = PluggedIn) -> UdecxUsbDevicePlugIn
 *   UNPLUG / owner handle closed / controller query-remove
 *           -> State = Gone, removed from tables, every pending request failed
 *              (DEVICE_GONE) -> UdecxUsbDevicePlugOutAndDelete -> reference dropped.
 * All of this is serialized by PF_CONTROLLER_CONTEXT::PlugLock (PASSIVE_LEVEL).
 */
#include "UsbDevice.h"
#include "Endpoint.h"
#include "RecordQueue.h"

namespace {

/* ------------------------------------------------------------------------ */
/* PLUGIN input parsing                                                     */
/* ------------------------------------------------------------------------ */

struct PF_DESCRIPTOR_VIEW {
    UINT8 Type;
    UINT8 Index;
    UINT16 LangId;
    ULONG Length;
    const UCHAR* Data;
};

struct PF_PLUGIN_SUMMARY {
    UINT64 Cookie;
    ULONG EntryCount;
    UINT8 Speed;
    BOOLEAN HasMsOsVendorCode;
    UINT8 MsOsVendorCode;
};

/*
 * Returns the entry at *Offset and advances *Offset past it (header + data padded
 * to 8 bytes). Every length is checked against the remaining input.
 */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfNextDescriptorEntry(_In_reads_bytes_(Length) const UCHAR* Base, size_t Length,
                                        _Inout_ size_t* Offset, _Out_ PF_DESCRIPTOR_VIEW* View)
{
    PAGED_CODE();

    RtlZeroMemory(View, sizeof(*View));
    if (*Offset > Length || Length - *Offset < sizeof(PFVHCI_DESCRIPTOR_ENTRY)) {
        return STATUS_INVALID_PARAMETER;
    }

    PFVHCI_DESCRIPTOR_ENTRY entry;
    RtlCopyMemory(&entry, Base + *Offset, sizeof(entry));

    if (entry.Length == 0 || entry.Length > PFVHCI_MAX_DESCRIPTOR_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }
    const ULONG64 total = sizeof(entry) + PfAlign8(entry.Length);
    if (total > Length - *Offset) {
        return STATUS_INVALID_PARAMETER;
    }

    View->Type = entry.Type;
    View->Index = entry.Index;
    View->LangId = entry.LangId;
    View->Length = entry.Length;
    View->Data = Base + *Offset + sizeof(entry);
    *Offset += static_cast<size_t>(total);
    return STATUS_SUCCESS;
}

/*
 * Walks the sub-descriptors of a configuration or BOS descriptor: every element
 * must have bLength >= MinElement and lie completely inside the descriptor.
 * UdeCx and usbhub3 parse these blobs, so they must be well formed.
 */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED BOOLEAN PfDescriptorChainIsValid(_In_reads_bytes_(Length) const UCHAR* Data, ULONG Length, UCHAR MinElement)
{
    PAGED_CODE();

    ULONG pos = Data[0]; /* skip the header descriptor itself (bLength checked by caller) */
    while (pos < Length) {
        if (Length - pos < 2) {
            return FALSE;
        }
        const UCHAR bLength = Data[pos];
        if (bLength < MinElement || bLength > Length - pos) {
            return FALSE;
        }
        if (Data[pos + 1] == PF_DESC_ENDPOINT && bLength < sizeof(USB_ENDPOINT_DESCRIPTOR)) {
            return FALSE;
        }
        pos += bLength;
    }
    return TRUE;
}

inline USHORT PfReadLe16(const UCHAR* p)
{
    return static_cast<USHORT>(p[0] | (p[1] << 8));
}

/* "MSFT100" in UTF-16LE: Microsoft OS 1.0 descriptor signature (string index 0xEE). */
const UCHAR PfMsOsSignature[14] = {'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0};

/*
 * Validates the complete PLUGIN input (never trust user mode). On success
 * Summary describes the device; nothing has been created yet.
 */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfValidatePlugin(_In_reads_bytes_(Length) const UCHAR* Input, size_t Length,
                                   _Out_ PF_PLUGIN_SUMMARY* Summary)
{
    PAGED_CODE();

    RtlZeroMemory(Summary, sizeof(*Summary));

    PFVHCI_PLUGIN_HEADER header;
    RtlCopyMemory(&header, Input, sizeof(header));

    if (header.Size != Length) {
        PF_WARN("PLUGIN: header.Size %u != input length %Iu\n", header.Size, Length);
        return STATUS_INVALID_PARAMETER;
    }
    if (header.AbiVersion != PFVHCI_ABI_VERSION) {
        PF_WARN("PLUGIN: ABI version %u, driver speaks %u\n", header.AbiVersion, PFVHCI_ABI_VERSION);
        return STATUS_REVISION_MISMATCH;
    }
    if (header.Speed < PFVHCI_SPEED_LOW || header.Speed > PFVHCI_SPEED_SUPER_PLUS) {
        return STATUS_INVALID_PARAMETER;
    }
    if (header.Reserved0[0] != 0 || header.Reserved0[1] != 0 || header.Reserved0[2] != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (header.EntryCount == 0 || header.EntryCount > PF_MAX_DESCRIPTOR_ENTRIES) {
        return STATUS_INVALID_PARAMETER;
    }

    BOOLEAN haveDevice = FALSE;
    BOOLEAN haveBos = FALSE;
    BOOLEAN haveQualifier = FALSE;
    UCHAR numConfigurations = 0;
    UCHAR configSeen[256 / 8];
    RtlZeroMemory(configSeen, sizeof(configSeen));
    ULONG configCount = 0;

    size_t offset = sizeof(PFVHCI_PLUGIN_HEADER); /* 24: already 8-aligned */
    for (ULONG i = 0; i < header.EntryCount; ++i) {
        PF_DESCRIPTOR_VIEW d;
        NTSTATUS status = PfNextDescriptorEntry(Input, Length, &offset, &d);
        if (!NT_SUCCESS(status)) {
            PF_WARN("PLUGIN: entry %u out of bounds\n", i);
            return status;
        }

        switch (d.Type) {
        case PF_DESC_DEVICE:
            if (haveDevice || d.Index != 0 || d.LangId != 0 || d.Length != sizeof(USB_DEVICE_DESCRIPTOR) ||
                d.Data[0] != sizeof(USB_DEVICE_DESCRIPTOR) || d.Data[1] != PF_DESC_DEVICE || d.Data[7] == 0) {
                PF_WARN("PLUGIN: bad device descriptor (entry %u)\n", i);
                return STATUS_INVALID_PARAMETER;
            }
            haveDevice = TRUE;
            numConfigurations = d.Data[17];
            break;

        case PF_DESC_CONFIGURATION:
            if (d.LangId != 0 || d.Length < sizeof(USB_CONFIGURATION_DESCRIPTOR) ||
                d.Data[0] != sizeof(USB_CONFIGURATION_DESCRIPTOR) || d.Data[1] != PF_DESC_CONFIGURATION ||
                PfReadLe16(d.Data + 2) != d.Length || !PfDescriptorChainIsValid(d.Data, d.Length, 2)) {
                PF_WARN("PLUGIN: bad configuration descriptor %u (entry %u)\n", d.Index, i);
                return STATUS_INVALID_PARAMETER;
            }
            if (configSeen[d.Index / 8] & (1u << (d.Index % 8))) {
                PF_WARN("PLUGIN: duplicate configuration index %u\n", d.Index);
                return STATUS_INVALID_PARAMETER;
            }
            configSeen[d.Index / 8] |= static_cast<UCHAR>(1u << (d.Index % 8));
            ++configCount;
            break;

        case PF_DESC_STRING:
            if (d.Length < 2 || d.Length > 255 || (d.Length & 1u) != 0 || d.Data[0] != d.Length ||
                d.Data[1] != PF_DESC_STRING) {
                PF_WARN("PLUGIN: bad string descriptor %u/0x%04X (entry %u)\n", d.Index, d.LangId, i);
                return STATUS_INVALID_PARAMETER;
            }
            if (d.Index == 0 && d.LangId != 0) {
                /*
                 * Index 0 is the LANGID list and has no language. Other strings normally
                 * carry a LANGID, but LangId 0 is accepted (the Microsoft OS string 0xEE
                 * is requested with wIndex 0).
                 */
                return STATUS_INVALID_PARAMETER;
            }
            if (d.Index == 0xEE && d.Length >= 18 &&
                RtlCompareMemory(d.Data + 2, PfMsOsSignature, sizeof(PfMsOsSignature)) == sizeof(PfMsOsSignature)) {
                Summary->HasMsOsVendorCode = TRUE;
                Summary->MsOsVendorCode = d.Data[16];
            }
            break;

        case PF_DESC_DEVICE_QUALIFIER:
            if (haveQualifier || d.Index != 0 || d.LangId != 0 || d.Length != sizeof(USB_DEVICE_QUALIFIER_DESCRIPTOR) ||
                d.Data[0] != sizeof(USB_DEVICE_QUALIFIER_DESCRIPTOR) || d.Data[1] != PF_DESC_DEVICE_QUALIFIER) {
                return STATUS_INVALID_PARAMETER;
            }
            haveQualifier = TRUE;
            break;

        case PF_DESC_BOS:
            if (haveBos || d.Index != 0 || d.LangId != 0 || d.Length < sizeof(USB_BOS_DESCRIPTOR) ||
                d.Data[0] != sizeof(USB_BOS_DESCRIPTOR) || d.Data[1] != PF_DESC_BOS ||
                PfReadLe16(d.Data + 2) != d.Length || !PfDescriptorChainIsValid(d.Data, d.Length, 3)) {
                PF_WARN("PLUGIN: bad BOS descriptor (entry %u)\n", i);
                return STATUS_INVALID_PARAMETER;
            }
            haveBos = TRUE;
            break;

        default:
            PF_WARN("PLUGIN: unsupported descriptor type 0x%02X (entry %u)\n", d.Type, i);
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (offset != Length) {
        PF_WARN("PLUGIN: %Iu trailing bytes\n", Length - offset);
        return STATUS_INVALID_PARAMETER;
    }
    if (!haveDevice || numConfigurations == 0 || configCount != numConfigurations) {
        PF_WARN("PLUGIN: need a device descriptor and all %u configurations (got %u)\n", numConfigurations,
                configCount);
        return STATUS_INVALID_PARAMETER;
    }
    for (ULONG c = 0; c < numConfigurations; ++c) {
        if ((configSeen[c / 8] & (1u << (c % 8))) == 0) {
            PF_WARN("PLUGIN: configuration index %u missing\n", c);
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (PfIsSuperSpeed(header.Speed) && !haveBos) {
        /* USB 3.x devices must have a BOS descriptor; usbhub3 fails enumeration otherwise. */
        PF_WARN("PLUGIN: SuperSpeed device without BOS descriptor\n");
        return STATUS_INVALID_PARAMETER;
    }

    Summary->Cookie = header.Cookie;
    Summary->EntryCount = header.EntryCount;
    Summary->Speed = header.Speed;
    return STATUS_SUCCESS;
}

/*
 * Feeds the (already validated) descriptors to UdeCx. UdeCx answers the standard
 * GET_DESCRIPTOR requests for these itself; class/vendor descriptors (HID report
 * etc.) still reach the endpoint queue and are forwarded to the remote device.
 *
 * VERIFY: whether UdeCx copies descriptor buffers during UdecxUsbDeviceInitAdd*.
 * The driver keeps its private copy of the whole input alive for the lifetime of
 * the device (PF_DEVICE_CONTEXT::Descriptors), so either behaviour is safe.
 * VERIFY: UdecxUsbDeviceInitAddDescriptorWithIndex is the right call for multiple
 * configuration descriptors and UdecxUsbDeviceInitAddDescriptor for the device
 * qualifier (the documentation only shows device/BOS/string-0 examples).
 */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfAddDescriptors(_Inout_ _UDECXUSBDEVICE_INIT* Init, _In_reads_bytes_(Length) const UCHAR* Input,
                                   size_t Length, ULONG EntryCount)
{
    PAGED_CODE();

    size_t offset = sizeof(PFVHCI_PLUGIN_HEADER);
    for (ULONG i = 0; i < EntryCount; ++i) {
        PF_DESCRIPTOR_VIEW d;
        NTSTATUS status = PfNextDescriptorEntry(Input, Length, &offset, &d);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        PUCHAR data = const_cast<PUCHAR>(d.Data);
        const USHORT length = static_cast<USHORT>(d.Length); /* <= 64 KiB - 1 for config/BOS (wTotalLength) */

        switch (d.Type) {
        case PF_DESC_DEVICE:
        case PF_DESC_BOS:
        case PF_DESC_DEVICE_QUALIFIER:
            status = UdecxUsbDeviceInitAddDescriptor(Init, data, length);
            break;

        case PF_DESC_CONFIGURATION:
            status = UdecxUsbDeviceInitAddDescriptorWithIndex(Init, data, length, d.Index);
            break;

        case PF_DESC_STRING:
            if (d.Index == 0) {
                /* Language ID list, added raw as in the UdeCx documentation sample. */
                status = UdecxUsbDeviceInitAddDescriptorWithIndex(Init, data, length, 0);
            } else {
                /*
                 * The UNICODE_STRING variant is used rather than ...AddStringDescriptorRaw
                 * because its input (characters only, no bLength/bDescriptorType header)
                 * is unambiguous. VERIFY on first build that UdeCx returns the string
                 * byte-for-byte (bString of the original descriptor).
                 */
                UNICODE_STRING string;
                string.Buffer = reinterpret_cast<PWCH>(data + 2);
                string.Length = static_cast<USHORT>(d.Length - 2);
                string.MaximumLength = string.Length;
                status = UdecxUsbDeviceInitAddStringDescriptor(Init, &string, d.Index, d.LangId);
            }
            break;

        default:
            status = STATUS_INVALID_PARAMETER; /* unreachable: validated before */
            break;
        }

        if (!NT_SUCCESS(status)) {
            PF_ERROR("PLUGIN: adding descriptor type 0x%02X index %u failed 0x%08X\n", d.Type, d.Index, status);
            return status;
        }
    }
    return STATUS_SUCCESS;
}

UDECX_USB_DEVICE_SPEED PfToUdecxSpeed(UINT8 Speed)
{
    switch (Speed) {
    case PFVHCI_SPEED_LOW:  return UdecxUsbLowSpeed;
    case PFVHCI_SPEED_FULL: return UdecxUsbFullSpeed;
    case PFVHCI_SPEED_HIGH: return UdecxUsbHighSpeed;
    default:                return UdecxUsbSuperSpeed; /* SUPER and SUPER_PLUS (UdeCx has no SuperSpeedPlus) */
    }
}

/* ------------------------------------------------------------------------ */
/* UDECXUSBDEVICE callbacks                                                 */
/* ------------------------------------------------------------------------ */

_Function_class_(EVT_WDF_OBJECT_CONTEXT_CLEANUP)
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfEvtUsbDeviceCleanup(_In_ WDFOBJECT Object)
{
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(Object);
    PF_TRACE("device %u (port %u) cleanup\n", dev->DeviceHandle, dev->Port);
    if (dev->Descriptors != nullptr) {
        WdfObjectDelete(dev->Descriptors);
        dev->Descriptors = nullptr;
    }
}

/* The remote device has no link power state to manage: accept every transition. */
_Function_class_(EVT_UDECX_USB_DEVICE_D0_ENTRY)
NTSTATUS PfEvtUsbDeviceLinkPowerEntry(_In_ WDFDEVICE UdecxWdfDevice, _In_ UDECXUSBDEVICE UdecxUsbDevice)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    PF_TRACE("device %u link power entry\n", PfGetDeviceContext(UdecxUsbDevice)->DeviceHandle);
    return STATUS_SUCCESS;
}

_Function_class_(EVT_UDECX_USB_DEVICE_D0_EXIT)
NTSTATUS PfEvtUsbDeviceLinkPowerExit(_In_ WDFDEVICE UdecxWdfDevice, _In_ UDECXUSBDEVICE UdecxUsbDevice,
                                     _In_ UDECX_USB_DEVICE_WAKE_SETTING WakeSetting)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    PF_TRACE("device %u link power exit (wake setting %d)\n", PfGetDeviceContext(UdecxUsbDevice)->DeviceHandle,
             static_cast<int>(WakeSetting));
    return STATUS_SUCCESS;
}

_Function_class_(EVT_UDECX_USB_DEVICE_SET_FUNCTION_SUSPEND_AND_WAKE)
NTSTATUS PfEvtUsbDeviceSetFunctionSuspendAndWake(_In_ WDFDEVICE UdecxWdfDevice, _In_ UDECXUSBDEVICE UdecxUsbDevice,
                                                 _In_ ULONG Interface,
                                                 _In_ UDECX_USB_DEVICE_FUNCTION_POWER FunctionPower)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    PF_TRACE("device %u function %u power %d\n", PfGetDeviceContext(UdecxUsbDevice)->DeviceHandle, Interface,
             static_cast<int>(FunctionPower));
    return STATUS_SUCCESS;
}

/*
 * Port reset requested after enumeration (e.g. IOCTL_INTERNAL_USB_RESET_PORT from a
 * function driver). Forwarded as PFVHCI_REC_DEVICE_RESET; completed on OP_RESULT.
 * VERIFY: the parameter list of EVT_UDECX_USB_DEVICE_POST_ENUMERATION_RESET is not
 * documented on Microsoft Learn; it is taken from usbip-win2. If it differs, the
 * assignment in PfCreateUdecxDevice fails to compile (no silent mismatch).
 */
_Function_class_(EVT_UDECX_USB_DEVICE_POST_ENUMERATION_RESET)
void PfEvtUsbDeviceReset(_In_ WDFDEVICE UdecxWdfDevice, _In_ UDECXUSBDEVICE UdecxUsbDevice, _In_ WDFREQUEST Request,
                         _In_ BOOLEAN AllDevicesReset)
{
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(UdecxWdfDevice);
    PF_INFO("device %u reset requested (all devices: %u)\n", PfGetDeviceContext(UdecxUsbDevice)->DeviceHandle,
            AllDevicesReset);

    PF_REQUEST_CONTEXT* ctx = PfRequestContextInit(Request, UdecxUsbDevice, nullptr);
    if (ctx == nullptr) {
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    ctx->RecKind = PfRecKind::DeviceReset;
    PfQueueRequestRecord(ctrl, ctx);
}

_Function_class_(EVT_UDECX_USB_DEVICE_DEFAULT_ENDPOINT_ADD)
NTSTATUS PfEvtUsbDeviceDefaultEndpointAdd(_In_ UDECXUSBDEVICE UdecxUsbDevice, _In_ _UDECXUSBENDPOINT_INIT* EndpointInit)
{
    _UDECXUSBENDPOINT_INIT* init = EndpointInit;
    return PfEndpointCreate(UdecxUsbDevice, &init, nullptr, 0);
}

_Function_class_(EVT_UDECX_USB_DEVICE_ENDPOINT_ADD)
NTSTATUS PfEvtUsbDeviceEndpointAdd(_In_ UDECXUSBDEVICE UdecxUsbDevice,
                                   _In_ UDECX_USB_ENDPOINT_INIT_AND_METADATA* EndpointToCreate)
{
    return PfEndpointCreate(UdecxUsbDevice, &EndpointToCreate->UdecxUsbEndpointInit,
                            EndpointToCreate->EndpointDescriptor, EndpointToCreate->EndpointDescriptorBufferLength);
}

/*
 * Configuration / alternate-setting changes. UdeCx completes SET_CONFIGURATION and
 * SET_INTERFACE itself (they never reach the endpoint queue), so the remote device
 * would never see them: they are forwarded as SELECT_CONFIG / SELECT_INTERFACE and
 * the request completes when user mode reports the OP_RESULT.
 *
 * VERIFY: usbip-win2 reports that UdeCx does not call DeviceConfigurationChange for
 * composite devices and passes wrong InterfaceNumber/NewInterfaceSetting values for
 * InterfaceSettingChange; it uses an upper filter driver that intercepts
 * URB_FUNCTION_SELECT_CONFIGURATION/SELECT_INTERFACE instead. Test composite devices
 * (ADB + MTP phones) early; if confirmed, add such a filter (ude_filter equivalent).
 */
_Function_class_(EVT_UDECX_USB_DEVICE_ENDPOINTS_CONFIGURE)
void PfEvtUsbDeviceEndpointsConfigure(_In_ UDECXUSBDEVICE UdecxUsbDevice, _In_ WDFREQUEST Request,
                                      _In_ UDECX_ENDPOINTS_CONFIGURE_PARAMS* Params)
{
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(UdecxUsbDevice);
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(dev->Controller);

    PfRecKind kind;
    switch (Params->ConfigureType) {
    case UdecxEndpointsConfigureTypeDeviceConfigurationChange:
        kind = PfRecKind::SelectConfig;
        break;
    case UdecxEndpointsConfigureTypeInterfaceSettingChange:
        kind = PfRecKind::SelectInterface;
        break;
    case UdecxEndpointsConfigureTypeDeviceInitialize:   /* internal to UdeCx */
    case UdecxEndpointsConfigureTypeEndpointsReleasedOnly:
    default:
        /*
         * Released endpoints: UdeCx treats their queues as purged and deletes the
         * UDECXUSBENDPOINT objects itself (deleting them here is reported to crash).
         */
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    PF_REQUEST_CONTEXT* ctx = PfRequestContextInit(Request, UdecxUsbDevice, nullptr);
    if (ctx == nullptr) {
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    ctx->RecKind = kind;
    ctx->OpConfigurationValue = static_cast<UINT8>(Params->NewConfigurationValue);
    ctx->OpInterfaceNumber = static_cast<UINT8>(Params->InterfaceNumber);
    ctx->OpAlternateSetting = static_cast<UINT8>(Params->NewInterfaceSetting);

    PF_TRACE("device %u %s cfg %u if %u alt %u\n", dev->DeviceHandle,
             kind == PfRecKind::SelectConfig ? "SELECT_CONFIG" : "SELECT_INTERFACE", ctx->OpConfigurationValue,
             ctx->OpInterfaceNumber, ctx->OpAlternateSetting);

    PfQueueRequestRecord(ctrl, ctx);
}

/* ------------------------------------------------------------------------ */
/* Creation                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * Creates the UDECXUSBDEVICE. On failure nothing is left behind except Memory,
 * which the caller still owns. On success Memory is owned by the device context.
 */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfCreateUdecxDevice(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                                      _In_ WDFMEMORY Memory, _In_reads_bytes_(Length) const UCHAR* Input, size_t Length,
                                      _In_ const PF_PLUGIN_SUMMARY* Summary, _Out_ UDECXUSBDEVICE* Device)
{
    PAGED_CODE();
    *Device = nullptr;

    _UDECXUSBDEVICE_INIT* init = UdecxUsbDeviceInitAllocate(Ctrl->Device);
    if (init == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS callbacks;
    UDECX_USB_DEVICE_CALLBACKS_INIT(&callbacks);
    callbacks.EvtUsbDeviceLinkPowerEntry = PfEvtUsbDeviceLinkPowerEntry;
    callbacks.EvtUsbDeviceLinkPowerExit = PfEvtUsbDeviceLinkPowerExit;
    if (PfIsSuperSpeed(Summary->Speed)) {
        /* Function suspend exists only for USB 3.x devices (usbip-win2 sets it only for them). */
        callbacks.EvtUsbDeviceSetFunctionSuspendAndWake = PfEvtUsbDeviceSetFunctionSuspendAndWake;
    }
    callbacks.EvtUsbDeviceReset = PfEvtUsbDeviceReset;
    callbacks.EvtUsbDeviceDefaultEndpointAdd = PfEvtUsbDeviceDefaultEndpointAdd;
    callbacks.EvtUsbDeviceEndpointAdd = PfEvtUsbDeviceEndpointAdd;
    callbacks.EvtUsbDeviceEndpointsConfigure = PfEvtUsbDeviceEndpointsConfigure;
    UdecxUsbDeviceInitSetStateChangeCallbacks(init, &callbacks);

    UdecxUsbDeviceInitSetSpeed(init, PfToUdecxSpeed(Summary->Speed));
    UdecxUsbDeviceInitSetEndpointsType(init, UdecxEndpointTypeDynamic);

    NTSTATUS status = PfAddDescriptors(init, Input, Length, Summary->EntryCount);
    if (!NT_SUCCESS(status)) {
        UdecxUsbDeviceInitFree(init);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PF_DEVICE_CONTEXT);
    attributes.ParentObject = Ctrl->Device;
    attributes.EvtCleanupCallback = PfEvtUsbDeviceCleanup;

    UDECXUSBDEVICE device = nullptr;
    status = UdecxUsbDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("UdecxUsbDeviceCreate 0x%08X\n", status);
        /* UdeCx clears *init on success only; on failure the init structure is still ours. */
        if (init != nullptr) {
            UdecxUsbDeviceInitFree(init);
        }
        return status;
    }

    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(device);
    dev->Handle = device;
    dev->Controller = Ctrl->Device;
    dev->Owner = File->FileObject;
    InitializeListHead(&dev->OwnerLink);
    InitializeListHead(&dev->Endpoints);
    dev->Descriptors = Memory;
    dev->Cookie = Summary->Cookie;
    dev->Speed = Summary->Speed;
    dev->State = PfDeviceState::Creating;
    dev->HasMsOsVendorCode = Summary->HasMsOsVendorCode;
    dev->MsOsVendorCode = Summary->MsOsVendorCode;

    *Device = device;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Port table / ownership (nonpaged: these take the spinlock)               */
/* ------------------------------------------------------------------------ */

_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG PfReservePort(_In_ PF_CONTROLLER_CONTEXT* Ctrl, BOOLEAN Usb3)
{
    const ULONG first = Usb3 ? Ctrl->NumUsb2Ports : 0;
    const ULONG end = Usb3 ? Ctrl->NumUsb2Ports + Ctrl->NumUsb3Ports : Ctrl->NumUsb2Ports;
    ULONG port = 0;

    WdfSpinLockAcquire(Ctrl->Lock);
    for (ULONG i = first; i < end && i < PF_MAX_PORTS; ++i) {
        if (Ctrl->Ports[i] == nullptr && !Ctrl->PortReserved[i]) {
            Ctrl->PortReserved[i] = TRUE;
            port = i + 1;
            break;
        }
    }
    WdfSpinLockRelease(Ctrl->Lock);
    return port;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void PfReleasePortReservation(_In_ PF_CONTROLLER_CONTEXT* Ctrl, ULONG Port)
{
    WdfSpinLockAcquire(Ctrl->Lock);
    Ctrl->PortReserved[Port - 1] = FALSE;
    WdfSpinLockRelease(Ctrl->Lock);
}

/*
 * Makes a freshly created device visible: assigns its handle, puts it into the
 * port table (taking a WDF reference) and the owner's list, and marks it
 * PluggedIn *before* UdecxUsbDevicePlugIn because URBs can arrive while that call
 * is still running.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS PfPublishDevice(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File, _In_ UDECXUSBDEVICE Device,
                         ULONG Port)
{
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(Device);
    NTSTATUS status = STATUS_SUCCESS;

    WdfSpinLockAcquire(Ctrl->Lock);
    Ctrl->PortReserved[Port - 1] = FALSE;
    if (File->Closing) {
        status = STATUS_DELETE_PENDING;
    } else {
        do {
            dev->DeviceHandle = ++Ctrl->NextDeviceHandle;
        } while (dev->DeviceHandle == 0);
        dev->Port = Port;
        dev->State = PfDeviceState::PluggedIn;
        Ctrl->Ports[Port - 1] = Device;
        WdfObjectReference(Device);
        InsertTailList(&File->Devices, &dev->OwnerLink);
        dev->OwnerLinked = TRUE;
    }
    WdfSpinLockRelease(Ctrl->Lock);
    return status;
}

/*
 * Takes a device out of service. Returns FALSE if somebody already did. Requests
 * that this caller now owns are appended to Owned (to be completed before the
 * device is plugged out). For a controller removal a DEVICE_REMOVED record is
 * queued for the owner (Record is consumed if *RecordUsed).
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN PfDetachDevice(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ UDECXUSBDEVICE Device,
                       _Inout_opt_ PF_STANDALONE_RECORD* Record, _Inout_ PLIST_ENTRY Owned,
                       _Out_ BOOLEAN* WasPluggedIn, _Out_ BOOLEAN* RecordUsed, _Out_ WDFFILEOBJECT* PumpFile)
{
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(Device);
    *WasPluggedIn = FALSE;
    *RecordUsed = FALSE;
    *PumpFile = nullptr;

    WdfSpinLockAcquire(Ctrl->Lock);
    if (dev->State == PfDeviceState::Gone) {
        WdfSpinLockRelease(Ctrl->Lock);
        return FALSE;
    }

    *WasPluggedIn = dev->State == PfDeviceState::PluggedIn;
    dev->State = PfDeviceState::Gone;

    if (dev->Port != 0 && Ctrl->Ports[dev->Port - 1] == Device) {
        Ctrl->Ports[dev->Port - 1] = nullptr; /* the table's WDF reference is dropped by the caller */
    }

    PF_FILE_CONTEXT* owner = dev->Owner != nullptr ? PfGetFileContext(dev->Owner) : nullptr;
    if (dev->OwnerLinked) {
        RemoveEntryList(&dev->OwnerLink);
        InitializeListHead(&dev->OwnerLink);
        dev->OwnerLinked = FALSE;
    }

    PfCollectDeviceRequestsLocked(Ctrl, owner, Device, Owned);

    if (owner != nullptr && Record != nullptr) {
        Record->Op.Header.RequestId = PfNextRequestIdLocked(Ctrl);
        if (PfInsertStandaloneLocked(Ctrl, owner, Record)) {
            *RecordUsed = TRUE;
            *PumpFile = dev->Owner;
            WdfObjectReference(*PumpFile);
        }
    }

    dev->Owner = nullptr;
    WdfSpinLockRelease(Ctrl->Lock);
    return TRUE;
}

/* Returns a referenced device (or NULL). Caller dereferences. */
_IRQL_requires_max_(DISPATCH_LEVEL)
UDECXUSBDEVICE PfReferenceDeviceByHandle(_In_ PF_CONTROLLER_CONTEXT* Ctrl, ULONG DeviceHandle,
                                         _Out_ WDFFILEOBJECT* Owner)
{
    UDECXUSBDEVICE found = nullptr;
    *Owner = nullptr;

    WdfSpinLockAcquire(Ctrl->Lock);
    for (ULONG i = 0; i < PF_MAX_PORTS; ++i) {
        UDECXUSBDEVICE device = Ctrl->Ports[i];
        if (device != nullptr && PfGetDeviceContext(device)->DeviceHandle == DeviceHandle) {
            found = device;
            *Owner = PfGetDeviceContext(device)->Owner;
            WdfObjectReference(found);
            break;
        }
    }
    WdfSpinLockRelease(Ctrl->Lock);
    return found;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
UDECXUSBDEVICE PfReferenceFirstOwnedDevice(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File)
{
    UDECXUSBDEVICE found = nullptr;
    WdfSpinLockAcquire(Ctrl->Lock);
    if (!IsListEmpty(&File->Devices)) {
        PF_DEVICE_CONTEXT* dev = CONTAINING_RECORD(File->Devices.Flink, PF_DEVICE_CONTEXT, OwnerLink);
        found = dev->Handle;
        WdfObjectReference(found);
    }
    WdfSpinLockRelease(Ctrl->Lock);
    return found;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
UDECXUSBDEVICE PfReferenceAnyDevice(_In_ PF_CONTROLLER_CONTEXT* Ctrl)
{
    UDECXUSBDEVICE found = nullptr;
    WdfSpinLockAcquire(Ctrl->Lock);
    for (ULONG i = 0; i < PF_MAX_PORTS; ++i) {
        if (Ctrl->Ports[i] != nullptr) {
            found = Ctrl->Ports[i];
            WdfObjectReference(found);
            break;
        }
    }
    WdfSpinLockRelease(Ctrl->Lock);
    return found;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void PfMarkFileClosing(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File)
{
    WdfSpinLockAcquire(Ctrl->Lock);
    File->Closing = TRUE;
    WdfSpinLockRelease(Ctrl->Lock);
}

/* ------------------------------------------------------------------------ */
/* Unplug                                                                   */
/* ------------------------------------------------------------------------ */

/* PlugLock must be held. Device must be referenced by the caller. */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfUnplugDeviceLocked(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ UDECXUSBDEVICE Device, PfUnplugReason Reason)
{
    PAGED_CODE();

    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(Device);
    const ULONG handle = dev->DeviceHandle;
    const ULONG port = dev->Port;

    PF_STANDALONE_RECORD* record = nullptr;
    if (Reason == PfUnplugReason::ControllerRemoval) {
        /* Informational: the owner did not ask for this. Not sent for UNPLUG or handle close. */
        record = PfAllocStandaloneRecord(Ctrl, PFVHCI_REC_DEVICE_REMOVED, handle, dev->Cookie, 0);
    }

    LIST_ENTRY owned;
    InitializeListHead(&owned);
    BOOLEAN wasPluggedIn = FALSE;
    BOOLEAN recordUsed = FALSE;
    WDFFILEOBJECT pumpFile = nullptr;

    const BOOLEAN detached = PfDetachDevice(Ctrl, Device, record, &owned, &wasPluggedIn, &recordUsed, &pumpFile);
    if (record != nullptr && !recordUsed) {
        PfFreeStandaloneRecord(record);
    }
    if (!detached) {
        return;
    }

    PF_INFO("unplugging device %u from port %u (reason %u)\n", handle, port, static_cast<unsigned>(Reason));

    /*
     * Fail what we own *before* plugging out (see the VERIFY note in RecordQueue.cpp
     * about error completions after UdecxUsbDevicePlugOutAndDelete). Requests that a
     * concurrent cancel routine or FETCH owns complete on their own shortly; UdeCx
     * purges the endpoints during plug-out and waits for them.
     */
    PfCompleteListNow(&owned);

    if (wasPluggedIn) {
        const NTSTATUS status = UdecxUsbDevicePlugOutAndDelete(Device);
        if (!NT_SUCCESS(status)) {
            PF_ERROR("UdecxUsbDevicePlugOutAndDelete(device %u) 0x%08X\n", handle, status);
        }
        /* UdeCx deletes the object; Device must not be passed to UdeCx again. */
    } else {
        WdfObjectDelete(Device);
    }

    /* Drop the port table's reference (the object memory lives until the last one goes). */
    WdfObjectDereference(Device);

    if (pumpFile != nullptr) {
        PfPumpFile(Ctrl, pumpFile);
        WdfObjectDereference(pumpFile);
    }
}

} // namespace

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfDevicePlugIn(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                                 _In_reads_bytes_(InputLength) const UCHAR* Input, _In_ size_t InputLength,
                                 _Out_ PFVHCI_PLUGIN_RESULT* Result)
{
    PAGED_CODE();
    RtlZeroMemory(Result, sizeof(*Result));

    if (InputLength < sizeof(PFVHCI_PLUGIN_HEADER)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (InputLength > PF_MAX_PLUGIN_BYTES) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    /*
     * Private nonpaged copy: parsing works on memory user mode cannot touch, the
     * system buffer can be reused for the output, and UdeCx may keep pointers into
     * the descriptors (see PfAddDescriptors).
     */
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctrl->Device;
    WDFMEMORY memory = nullptr;
    PVOID copy = nullptr;
    NTSTATUS status = WdfMemoryCreate(&attributes, NonPagedPoolNx, PF_POOL_TAG, InputLength, &memory, &copy);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlCopyMemory(copy, Input, InputLength);
    const UCHAR* data = static_cast<const UCHAR*>(copy);

    PF_PLUGIN_SUMMARY summary;
    status = PfValidatePlugin(data, InputLength, &summary);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(memory);
        return status;
    }

    WdfWaitLockAcquire(Ctrl->PlugLock, nullptr);

    ULONG port = 0;
    UDECXUSBDEVICE device = nullptr;

    if (File->Closing) {
        status = STATUS_DELETE_PENDING;
    } else {
        port = PfReservePort(Ctrl, PfIsSuperSpeed(summary.Speed));
        if (port == 0) {
            PF_WARN("PLUGIN: no free %s port\n", PfIsSuperSpeed(summary.Speed) ? "USB 3" : "USB 2");
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (NT_SUCCESS(status)) {
        status = PfCreateUdecxDevice(Ctrl, File, memory, data, InputLength, &summary, &device);
        if (!NT_SUCCESS(status)) {
            PfReleasePortReservation(Ctrl, port);
        } else {
            memory = nullptr; /* now owned by the device context */
        }
    }

    if (NT_SUCCESS(status)) {
        status = PfPublishDevice(Ctrl, File, device, port);
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(device); /* never plugged in: plain delete is allowed */
            device = nullptr;
        }
    }

    if (NT_SUCCESS(status)) {
        UDECX_USB_DEVICE_PLUG_IN_OPTIONS options;
        UDECX_USB_DEVICE_PLUG_IN_OPTIONS_INIT(&options);
        /*
         * VERIFY: port numbering. usbip-win2 (production UdeCx driver) passes the
         * global root-hub port number: USB 2 ports 1..N2, USB 3 ports N2+1..N2+N3,
         * in Usb20PortNumber or Usb30PortNumber respectively. The same scheme is used here.
         */
        if (PfIsSuperSpeed(summary.Speed)) {
            options.Usb30PortNumber = port;
        } else {
            options.Usb20PortNumber = port;
        }

        status = UdecxUsbDevicePlugIn(device, &options);
        if (!NT_SUCCESS(status)) {
            PF_ERROR("UdecxUsbDevicePlugIn(port %u) 0x%08X\n", port, status);
            /* Take it back out of the tables; PlugIn failed, so WdfObjectDelete (not plug-out) applies. */
            LIST_ENTRY owned;
            InitializeListHead(&owned);
            BOOLEAN wasPluggedIn;
            BOOLEAN recordUsed;
            WDFFILEOBJECT pumpFile;
            PfDetachDevice(Ctrl, device, nullptr, &owned, &wasPluggedIn, &recordUsed, &pumpFile);
            PfCompleteListNow(&owned);
            WdfObjectDelete(device);
            WdfObjectDereference(device); /* table reference */
            device = nullptr;
        }
    }

    if (NT_SUCCESS(status)) {
        PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(device);
        Result->DeviceHandle = dev->DeviceHandle;
        Result->PortNumber = port;
        PF_INFO("device %u plugged into port %u (speed %u)\n", dev->DeviceHandle, port, summary.Speed);
    }

    WdfWaitLockRelease(Ctrl->PlugLock);

    if (memory != nullptr) {
        WdfObjectDelete(memory);
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfDeviceUnplugByHandle(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                                         _In_ ULONG DeviceHandle)
{
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;

    WdfWaitLockAcquire(Ctrl->PlugLock, nullptr);

    if (DeviceHandle == 0) {
        UDECXUSBDEVICE device;
        while ((device = PfReferenceFirstOwnedDevice(Ctrl, File)) != nullptr) {
            PfUnplugDeviceLocked(Ctrl, device, PfUnplugReason::UserRequest);
            WdfObjectDereference(device);
        }
    } else {
        WDFFILEOBJECT owner = nullptr;
        UDECXUSBDEVICE device = PfReferenceDeviceByHandle(Ctrl, DeviceHandle, &owner);
        if (device == nullptr) {
            status = STATUS_NOT_FOUND;
        } else {
            if (owner != File->FileObject) {
                status = STATUS_ACCESS_DENIED; /* devices belong to the handle that plugged them */
            } else {
                PfUnplugDeviceLocked(Ctrl, device, PfUnplugReason::UserRequest);
            }
            WdfObjectDereference(device);
        }
    }

    WdfWaitLockRelease(Ctrl->PlugLock);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfDeviceUnplugAllOwnedBy(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File)
{
    PAGED_CODE();

    WdfWaitLockAcquire(Ctrl->PlugLock, nullptr);

    /* From now on: no new PLUGIN, no new records, new URBs of its devices fail. */
    PfMarkFileClosing(Ctrl, File);

    UDECXUSBDEVICE device;
    while ((device = PfReferenceFirstOwnedDevice(Ctrl, File)) != nullptr) {
        PfUnplugDeviceLocked(Ctrl, device, PfUnplugReason::OwnerClosed);
        WdfObjectDereference(device);
    }

    WdfWaitLockRelease(Ctrl->PlugLock);
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfDeviceUnplugAll(_In_ PF_CONTROLLER_CONTEXT* Ctrl)
{
    PAGED_CODE();

    WdfWaitLockAcquire(Ctrl->PlugLock, nullptr);
    UDECXUSBDEVICE device;
    while ((device = PfReferenceAnyDevice(Ctrl)) != nullptr) {
        PfUnplugDeviceLocked(Ctrl, device, PfUnplugReason::ControllerRemoval);
        WdfObjectDereference(device);
    }
    WdfWaitLockRelease(Ctrl->PlugLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS PfDeviceSnapshotPorts(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                               _Out_writes_bytes_(OutputLength) PUCHAR Output, _In_ size_t OutputLength,
                               _Out_ size_t* Written)
{
    *Written = 0;
    const size_t headerSize = FIELD_OFFSET(PFVHCI_PORT_LIST, Ports);
    if (OutputLength < headerSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    const ULONG count = Ctrl->NumUsb2Ports + Ctrl->NumUsb3Ports; /* fixed after start */
    const size_t required = headerSize + static_cast<size_t>(count) * sizeof(PFVHCI_PORT_ENTRY);

    auto list = reinterpret_cast<PFVHCI_PORT_LIST*>(Output);
    list->Count = count;
    list->Reserved = 0;

    if (OutputLength < required) {
        *Written = headerSize; /* caller retries with headerSize + Count * sizeof(PFVHCI_PORT_ENTRY) */
        return STATUS_BUFFER_OVERFLOW;
    }

    PFVHCI_PORT_ENTRY* entries = reinterpret_cast<PFVHCI_PORT_ENTRY*>(Output + headerSize);
    RtlZeroMemory(entries, static_cast<size_t>(count) * sizeof(PFVHCI_PORT_ENTRY));

    WdfSpinLockAcquire(Ctrl->Lock);
    for (ULONG i = 0; i < count; ++i) {
        entries[i].PortNumber = i + 1;
        UDECXUSBDEVICE device = Ctrl->Ports[i];
        if (device != nullptr) {
            const PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(device);
            entries[i].DeviceHandle = dev->DeviceHandle;
            entries[i].Cookie = dev->Owner == File->FileObject ? dev->Cookie : 0;
            entries[i].Speed = dev->Speed;
        } else {
            /* Speed of an empty port = the port's type, so user mode can tell USB 2 from USB 3 ports. */
            entries[i].Speed = static_cast<UINT8>(i < Ctrl->NumUsb2Ports ? PFVHCI_SPEED_HIGH : PFVHCI_SPEED_SUPER);
        }
    }
    WdfSpinLockRelease(Ctrl->Lock);

    *Written = required;
    return STATUS_SUCCESS;
}
