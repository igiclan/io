/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Endpoint.cpp - UDECXUSBENDPOINT callbacks and URB -> PFVHCI_SUBMIT translation.
 *
 * Every endpoint has its own WDF queue (created on the controller WDFDEVICE,
 * parented to the endpoint). Control endpoints use sequential dispatch (one
 * control transfer at a time, like a real pipe); bulk, interrupt and isochronous
 * endpoints use parallel dispatch so that many URBs can be in flight at once
 * (adb/fastboot keep several bulk reads pending).
 *
 * UdeCx answers standard GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION /
 * SET_INTERFACE / CLEAR_FEATURE(ENDPOINT_HALT)... itself (see the "Write a UDE
 * client driver" documentation), and the USB stack above it converts most
 * request-style URBs into URB_FUNCTION_CONTROL_TRANSFER(_EX) before they reach
 * us. The request-style URB functions are nevertheless translated here so that
 * nothing a function driver can send is silently dropped.
 */
#include "Endpoint.h"
#include "RecordQueue.h"

namespace {

void PfSetSetup(_Out_writes_(8) UCHAR* Setup, UCHAR RequestType, UCHAR Request, USHORT Value, USHORT Index,
                USHORT Length)
{
    Setup[0] = RequestType;
    Setup[1] = Request;
    Setup[2] = static_cast<UCHAR>(Value & 0xFF);
    Setup[3] = static_cast<UCHAR>(Value >> 8);
    Setup[4] = static_cast<UCHAR>(Index & 0xFF);
    Setup[5] = static_cast<UCHAR>(Index >> 8);
    Setup[6] = static_cast<UCHAR>(Length & 0xFF);
    Setup[7] = static_cast<UCHAR>(Length >> 8);
}

/*
 * Maps the URB transfer buffer (UdecxUrbRetrieveBuffer handles both TransferBuffer
 * and TransferBufferMDL) and bounds the transfer length by the mapped size and
 * PFVHCI_MAX_TRANSFER_BYTES.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS PfAttachBuffer(_Inout_ PF_REQUEST_CONTEXT* Ctx, ULONG Requested)
{
    Ctx->Buffer = nullptr;
    Ctx->TransferLength = 0;
    if (Requested == 0) {
        return STATUS_SUCCESS;
    }
    if (Requested > PFVHCI_MAX_TRANSFER_BYTES) {
        PF_WARN("URB transfer of %u bytes exceeds PFVHCI_MAX_TRANSFER_BYTES\n", Requested);
        return STATUS_INVALID_PARAMETER;
    }

    PUCHAR buffer = nullptr;
    ULONG length = 0;
    const NTSTATUS status = UdecxUrbRetrieveBuffer(Ctx->Request, &buffer, &length);
    if (!NT_SUCCESS(status)) {
        PF_WARN("UdecxUrbRetrieveBuffer 0x%08X\n", status);
        return status;
    }
    if (buffer == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    Ctx->Buffer = buffer;
    Ctx->TransferLength = Requested <= length ? Requested : length;
    return STATUS_SUCCESS;
}

/*
 * Control SUBMIT from an 8-byte setup packet. The data stage never exceeds the URB
 * buffer: if TransferBufferLength < wLength, wLength is lowered so that the remote
 * device cannot return more than the URB can hold.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS PfPrepareControl(_Inout_ PF_REQUEST_CONTEXT* Ctx, UCHAR EndpointNumber, _In_reads_(8) const UCHAR* Setup,
                          ULONG TransferBufferLength, ULONG TimeoutMs, BOOLEAN HasBuffer)
{
    RtlCopyMemory(Ctx->Setup, Setup, sizeof(Ctx->Setup));
    const BOOLEAN dirIn = (Setup[0] & PF_RT_DIR_IN) != 0;
    const ULONG wLength = static_cast<ULONG>(Setup[6]) | (static_cast<ULONG>(Setup[7]) << 8);

    const ULONG length = HasBuffer ? (TransferBufferLength < wLength ? TransferBufferLength : wLength) : 0;
    const NTSTATUS status = PfAttachBuffer(Ctx, length);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    Ctx->Setup[6] = static_cast<UCHAR>(Ctx->TransferLength & 0xFF);
    Ctx->Setup[7] = static_cast<UCHAR>((Ctx->TransferLength >> 8) & 0xFF);

    Ctx->RecKind = PfRecKind::Submit;
    Ctx->XferType = PFVHCI_XFER_CONTROL;
    Ctx->DirIn = dirIn;
    Ctx->XferFlags = static_cast<UINT16>(dirIn ? PFVHCI_XF_DIR_IN : 0);
    Ctx->EpAddress = static_cast<UINT8>((EndpointNumber & 0x0F) | (dirIn ? 0x80 : 0x00));
    Ctx->TimeoutMs = TimeoutMs;
    Ctx->SetBytes = HasBuffer;
    return STATUS_SUCCESS;
}

/* Request-style URB (GET_STATUS, VENDOR_DEVICE, ...) -> control transfer on the default pipe. */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS PfPrepareRequest(_Inout_ PF_REQUEST_CONTEXT* Ctx, UCHAR RequestType, UCHAR Request, USHORT Value,
                          USHORT Index, ULONG TransferBufferLength, BOOLEAN HasBuffer)
{
    const USHORT length = static_cast<USHORT>(HasBuffer ? (TransferBufferLength > 0xFFFF ? 0xFFFF : TransferBufferLength) : 0);
    UCHAR setup[8];
    PfSetSetup(setup, RequestType, Request, Value, Index, length);
    return PfPrepareControl(Ctx, 0, setup, length, 0, HasBuffer);
}

/*
 * Finds the endpoint a pipe-level URB (ABORT_PIPE, SYNC_RESET_PIPE...) refers to.
 * Pipe handles are learned from transfer URBs; unknown handles fall back to the
 * endpoint whose queue received the URB. Returns a referenced endpoint.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
UDECXUSBENDPOINT PfReferenceEndpointByPipe(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_DEVICE_CONTEXT* Dev,
                                           _In_opt_ USBD_PIPE_HANDLE Pipe, _In_ UDECXUSBENDPOINT Fallback)
{
    UDECXUSBENDPOINT found = Fallback;
    WdfSpinLockAcquire(Ctrl->Lock);
    if (Pipe != nullptr) {
        for (PLIST_ENTRY link = Dev->Endpoints.Flink; link != &Dev->Endpoints; link = link->Flink) {
            PF_ENDPOINT_CONTEXT* ep = CONTAINING_RECORD(link, PF_ENDPOINT_CONTEXT, DeviceLink);
            if (ep->PipeHandle == Pipe) {
                found = ep->Handle;
                break;
            }
        }
    }
    WdfObjectReference(found);
    WdfSpinLockRelease(Ctrl->Lock);
    return found;
}

inline void PfLearnPipe(_Inout_ PF_ENDPOINT_CONTEXT* Ep, USBD_PIPE_HANDLE Pipe)
{
    /* Pointer-sized store; readers take the lock and tolerate a stale value. */
    if (Pipe != nullptr && Ep->PipeHandle != Pipe) {
        Ep->PipeHandle = Pipe;
    }
}

/*
 * Translates one URB into a record (or handles it locally) and either queues it or
 * completes it through the DPC. Never completes on the caller's thread.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfProcessUrb(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ UDECXUSBENDPOINT Endpoint, _Inout_ PF_REQUEST_CONTEXT* Ctx)
{
    PF_ENDPOINT_CONTEXT* ep = PfGetEndpointContext(Endpoint);
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(ep->Device);
    PURB urb = Ctx->Urb;

    NTSTATUS status = STATUS_SUCCESS;
    USBD_STATUS failUsbd = USBD_STATUS_INVALID_PARAMETER;
    const USHORT function = urb->UrbHeader.Function;

    Ctx->RecKind = PfRecKind::Submit;

    switch (function) {

    /* ---- transfers ---- */

    case URB_FUNCTION_CONTROL_TRANSFER: {
        _URB_CONTROL_TRANSFER& r = urb->UrbControlTransfer;
        if (ep->TransferType != PFVHCI_XFER_CONTROL) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PfLearnPipe(ep, r.PipeHandle);
        status = PfPrepareControl(Ctx, ep->Address, r.SetupPacket, r.TransferBufferLength, 0, TRUE);
        break;
    }

    case URB_FUNCTION_CONTROL_TRANSFER_EX: {
        _URB_CONTROL_TRANSFER_EX& r = urb->UrbControlTransferEx;
        if (ep->TransferType != PFVHCI_XFER_CONTROL) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PfLearnPipe(ep, r.PipeHandle);
        status = PfPrepareControl(Ctx, ep->Address, r.SetupPacket, r.TransferBufferLength, r.Timeout, TRUE);
        break;
    }

    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL: {
        /*
         * Chained MDLs are reported as unsupported (EvtUdecxWdfDeviceQueryUsbCapability),
         * so the _USING_CHAINED_MDL variant should not arrive; if it does,
         * UdecxUrbRetrieveBuffer decides whether the buffer can be mapped.
         */
        _URB_BULK_OR_INTERRUPT_TRANSFER& r = urb->UrbBulkOrInterruptTransfer;
        if (ep->TransferType != PFVHCI_XFER_BULK && ep->TransferType != PFVHCI_XFER_INTERRUPT) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PfLearnPipe(ep, r.PipeHandle);
        status = PfAttachBuffer(Ctx, r.TransferBufferLength);
        if (!NT_SUCCESS(status)) {
            break;
        }
        /* Direction comes from the endpoint address; USBD_TRANSFER_DIRECTION_IN is advisory for bulk. */
        Ctx->DirIn = (ep->Address & 0x80) != 0;
        Ctx->XferType = ep->TransferType;
        Ctx->XferFlags = static_cast<UINT16>(Ctx->DirIn ? PFVHCI_XF_DIR_IN : 0);
        /*
         * PFVHCI_XF_SHORT_NOT_OK is deliberately never set: the Windows xHCI stack
         * treats short IN transfers as success whether or not USBD_SHORT_TRANSFER_OK
         * is set, and function drivers are written against that behaviour.
         * Windows has no URB flag for zero-length-packet termination (WinUSB sends an
         * explicit zero-length transfer), so PFVHCI_XF_ZERO_PACKET is not set either.
         */
        Ctx->EpAddress = ep->Address;
        Ctx->SetBytes = TRUE;
        break;
    }

    case URB_FUNCTION_ISOCH_TRANSFER:
    case URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL: {
        _URB_ISOCH_TRANSFER& r = urb->UrbIsochronousTransfer;
        if (ep->TransferType != PFVHCI_XFER_ISOCHRONOUS) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PfLearnPipe(ep, r.PipeHandle);
        const ULONG packets = r.NumberOfPackets;
        if (packets == 0 || packets > PFVHCI_MAX_ISO_PACKETS) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = PfAttachBuffer(Ctx, r.TransferBufferLength);
        if (!NT_SUCCESS(status)) {
            break;
        }
        ULONG previous = 0;
        for (ULONG i = 0; i < packets; ++i) {
            const ULONG offset = r.IsoPacket[i].Offset;
            if (offset < previous || offset > Ctx->TransferLength) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            previous = offset;
        }
        if (!NT_SUCCESS(status)) {
            break;
        }
        /*
         * StartFrame / USBD_START_ISO_TRANSFER_ASAP have no equivalent in the ABI: the
         * remote side schedules the transfer as soon as possible.
         */
        Ctx->DirIn = (ep->Address & 0x80) != 0;
        Ctx->XferType = PFVHCI_XFER_ISOCHRONOUS;
        Ctx->XferFlags = static_cast<UINT16>(Ctx->DirIn ? PFVHCI_XF_DIR_IN : 0);
        Ctx->EpAddress = ep->Address;
        Ctx->IsoPacketCount = packets;
        Ctx->SetBytes = TRUE;
        r.ErrorCount = 0;
        break;
    }

    /* ---- standard requests ---- */

    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT: {
        _URB_CONTROL_DESCRIPTOR_REQUEST& r = urb->UrbControlDescriptorRequest;
        const UCHAR recipient = function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE      ? PF_RT_RECIP_DEVICE
                                : function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE ? PF_RT_RECIP_INTERFACE
                                                                                         : PF_RT_RECIP_ENDPOINT;
        /* For interface/endpoint recipients LanguageId carries wIndex (e.g. HID report descriptors). */
        status = PfPrepareRequest(Ctx, static_cast<UCHAR>(PF_RT_DIR_IN | PF_RT_TYPE_STANDARD | recipient), PF_REQ_GET_DESCRIPTOR,
                                  static_cast<USHORT>((r.DescriptorType << 8) | r.Index), r.LanguageId,
                                  r.TransferBufferLength, TRUE);
        break;
    }

    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT: {
        _URB_CONTROL_DESCRIPTOR_REQUEST& r = urb->UrbControlDescriptorRequest;
        const UCHAR recipient = function == URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE      ? PF_RT_RECIP_DEVICE
                                : function == URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE ? PF_RT_RECIP_INTERFACE
                                                                                       : PF_RT_RECIP_ENDPOINT;
        status = PfPrepareRequest(Ctx, static_cast<UCHAR>(PF_RT_TYPE_STANDARD | recipient), PF_REQ_SET_DESCRIPTOR,
                                  static_cast<USHORT>((r.DescriptorType << 8) | r.Index), r.LanguageId,
                                  r.TransferBufferLength, TRUE);
        break;
    }

    case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
    case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_SET_FEATURE_TO_OTHER:
    case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER: {
        _URB_CONTROL_FEATURE_REQUEST& r = urb->UrbControlFeatureRequest;
        UCHAR recipient;
        BOOLEAN set;
        switch (function) {
        case URB_FUNCTION_SET_FEATURE_TO_DEVICE:     recipient = PF_RT_RECIP_DEVICE;    set = TRUE;  break;
        case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:  recipient = PF_RT_RECIP_INTERFACE; set = TRUE;  break;
        case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:   recipient = PF_RT_RECIP_ENDPOINT;  set = TRUE;  break;
        case URB_FUNCTION_SET_FEATURE_TO_OTHER:      recipient = PF_RT_RECIP_OTHER;     set = TRUE;  break;
        case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:   recipient = PF_RT_RECIP_DEVICE;    set = FALSE; break;
        case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:recipient = PF_RT_RECIP_INTERFACE; set = FALSE; break;
        case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT: recipient = PF_RT_RECIP_ENDPOINT;  set = FALSE; break;
        default:                                     recipient = PF_RT_RECIP_OTHER;     set = FALSE; break;
        }
        status = PfPrepareRequest(Ctx, static_cast<UCHAR>(PF_RT_TYPE_STANDARD | recipient),
                                  set ? PF_REQ_SET_FEATURE : PF_REQ_CLEAR_FEATURE, r.FeatureSelector, r.Index, 0,
                                  FALSE);
        break;
    }

    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_STATUS_FROM_OTHER: {
        _URB_CONTROL_GET_STATUS_REQUEST& r = urb->UrbControlGetStatusRequest;
        const UCHAR recipient = function == URB_FUNCTION_GET_STATUS_FROM_DEVICE      ? PF_RT_RECIP_DEVICE
                                : function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE ? PF_RT_RECIP_INTERFACE
                                : function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT  ? PF_RT_RECIP_ENDPOINT
                                                                                     : PF_RT_RECIP_OTHER;
        status = PfPrepareRequest(Ctx, static_cast<UCHAR>(PF_RT_DIR_IN | PF_RT_TYPE_STANDARD | recipient), PF_REQ_GET_STATUS, 0, r.Index,
                                  r.TransferBufferLength, TRUE);
        break;
    }

    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
    case URB_FUNCTION_VENDOR_OTHER:
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_CLASS_OTHER: {
        _URB_CONTROL_VENDOR_OR_CLASS_REQUEST& r = urb->UrbControlVendorClassRequest;
        UCHAR type;
        UCHAR recipient;
        switch (function) {
        case URB_FUNCTION_VENDOR_DEVICE:    type = PF_RT_TYPE_VENDOR; recipient = PF_RT_RECIP_DEVICE;    break;
        case URB_FUNCTION_VENDOR_INTERFACE: type = PF_RT_TYPE_VENDOR; recipient = PF_RT_RECIP_INTERFACE; break;
        case URB_FUNCTION_VENDOR_ENDPOINT:  type = PF_RT_TYPE_VENDOR; recipient = PF_RT_RECIP_ENDPOINT;  break;
        case URB_FUNCTION_VENDOR_OTHER:     type = PF_RT_TYPE_VENDOR; recipient = PF_RT_RECIP_OTHER;     break;
        case URB_FUNCTION_CLASS_DEVICE:     type = PF_RT_TYPE_CLASS;  recipient = PF_RT_RECIP_DEVICE;    break;
        case URB_FUNCTION_CLASS_INTERFACE:  type = PF_RT_TYPE_CLASS;  recipient = PF_RT_RECIP_INTERFACE; break;
        case URB_FUNCTION_CLASS_ENDPOINT:   type = PF_RT_TYPE_CLASS;  recipient = PF_RT_RECIP_ENDPOINT;  break;
        default:                            type = PF_RT_TYPE_CLASS;  recipient = PF_RT_RECIP_OTHER;     break;
        }
        const BOOLEAN dirIn = (r.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
        const UCHAR requestType =
            static_cast<UCHAR>((dirIn ? PF_RT_DIR_IN : 0) | type | recipient | r.RequestTypeReservedBits);
        status = PfPrepareRequest(Ctx, requestType, r.Request, r.Value, r.Index, r.TransferBufferLength, TRUE);
        break;
    }

    case URB_FUNCTION_GET_CONFIGURATION: {
        _URB_CONTROL_GET_CONFIGURATION_REQUEST& r = urb->UrbControlGetConfigurationRequest;
        status = PfPrepareRequest(Ctx, static_cast<UCHAR>(PF_RT_DIR_IN | PF_RT_TYPE_STANDARD | PF_RT_RECIP_DEVICE),
                                  PF_REQ_GET_CONFIGURATION, 0, 0, r.TransferBufferLength, TRUE);
        break;
    }

    case URB_FUNCTION_GET_INTERFACE: {
        _URB_CONTROL_GET_INTERFACE_REQUEST& r = urb->UrbControlGetInterfaceRequest;
        status = PfPrepareRequest(Ctx, static_cast<UCHAR>(PF_RT_DIR_IN | PF_RT_TYPE_STANDARD | PF_RT_RECIP_INTERFACE),
                                  PF_REQ_GET_INTERFACE, 0, r.Interface, r.TransferBufferLength, TRUE);
        break;
    }

    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR: {
        /*
         * Microsoft OS 1.0 feature descriptor: a vendor request using bMS_VendorCode
         * from string descriptor 0xEE (captured at PLUGIN).
         * VERIFY: _URB_OS_FEATURE_DESCRIPTOR_REQUEST member names against usb.h.
         */
        _URB_OS_FEATURE_DESCRIPTOR_REQUEST& r = urb->UrbOSFeatureDescriptorRequest;
        if (!dev->HasMsOsVendorCode) {
            status = STATUS_NOT_SUPPORTED;
            failUsbd = USBD_STATUS_STALL_PID;
            break;
        }
        const UCHAR recipient = static_cast<UCHAR>(r.Recipient & 0x1F);
        status = PfPrepareRequest(Ctx, static_cast<UCHAR>(PF_RT_DIR_IN | PF_RT_TYPE_VENDOR | recipient),
                                  dev->MsOsVendorCode,
                                  static_cast<USHORT>((r.InterfaceNumber << 8) | r.MS_PageIndex),
                                  r.MS_FeatureDescriptorIndex, r.TransferBufferLength, TRUE);
        break;
    }

    /* ---- pipe requests (normally handled by UdeCx through EvtUsbEndpointReset/Purge) ---- */

    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
    case URB_FUNCTION_SYNC_RESET_PIPE:
    case URB_FUNCTION_SYNC_CLEAR_STALL: {
        UDECXUSBENDPOINT target = PfReferenceEndpointByPipe(Ctrl, dev, urb->UrbPipeRequest.PipeHandle, Endpoint);
        Ctx->RecKind = PfRecKind::EndpointReset;
        Ctx->OpEndpoint = PfGetEndpointContext(target)->Address;
        Ctx->SetBytes = FALSE;
        WdfObjectDereference(target);
        break;
    }

    case URB_FUNCTION_ABORT_PIPE: {
        UDECXUSBENDPOINT target = PfReferenceEndpointByPipe(Ctrl, dev, urb->UrbPipeRequest.PipeHandle, Endpoint);
        PfAbortEndpointRequests(Ctrl, ep->Device, target);
        WdfObjectDereference(target);
        PfCompleteAsync(Ctrl, Ctx, STATUS_SUCCESS, USBD_STATUS_SUCCESS, 0);
        return;
    }

    default:
        /* SELECT_CONFIGURATION/SELECT_INTERFACE are turned into EvtUsbDeviceEndpointsConfigure by UdeCx. */
        PF_WARN("unsupported URB function 0x%04X on endpoint 0x%02X\n", function, ep->Address);
        status = STATUS_NOT_SUPPORTED;
        failUsbd = USBD_STATUS_INVALID_URB_FUNCTION;
        break;
    }

    if (!NT_SUCCESS(status)) {
        PfCompleteAsync(Ctrl, Ctx, status, failUsbd, 0);
        return;
    }

    PF_URB_TRACE("URB fn 0x%04X ep 0x%02X len %u\n", function, Ctx->EpAddress, Ctx->TransferLength);
    PfQueueRequestRecord(Ctrl, Ctx);
}

/* ------------------------------------------------------------------------ */
/* Queue callbacks                                                          */
/* ------------------------------------------------------------------------ */

_Function_class_(EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL)
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfEvtEndpointIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                                          _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    const UDECXUSBENDPOINT endpoint = PfGetQueueContext(Queue)->Endpoint;
    PF_ENDPOINT_CONTEXT* ep = PfGetEndpointContext(endpoint);
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(WdfIoQueueGetDevice(Queue));

    PF_REQUEST_CONTEXT* ctx = PfRequestContextInit(Request, ep->Device, endpoint);
    if (ctx == nullptr) {
        /* Low-resource fallback: cannot use the DPC without a context. */
        PfCompleteRequestWithoutContext(Request, STATUS_INSUFFICIENT_RESOURCES, USBD_STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    /*
     * Snapshot of the endpoint's abort sequence at dispatch time; PfQueueRequestRecord
     * cancels the URB if a purge/ABORT_PIPE happened while it was being translated.
     * An unlocked read is fine: a stale value can only cause an extra cancellation of
     * a request that raced with a purge.
     */
    ctx->AbortSequence = ep->AbortSequence;

    if (IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB || ctx->Urb == nullptr) {
        PF_WARN("endpoint queue: unexpected internal IOCTL 0x%08X\n", IoControlCode);
        PfCompleteAsync(ctrl, ctx, STATUS_INVALID_DEVICE_REQUEST, USBD_STATUS_INVALID_PARAMETER, 0);
        return;
    }

    PfProcessUrb(ctrl, endpoint, ctx);
}

_Function_class_(EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE)
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfEvtEndpointCanceledOnQueue(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request)
{
    /* UdeCx best practice: complete canceled URBs on a separate DPC, not on the caller's thread. */
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(WdfIoQueueGetDevice(Queue));
    const UDECXUSBENDPOINT endpoint = PfGetQueueContext(Queue)->Endpoint;
    PF_REQUEST_CONTEXT* ctx = PfRequestContextInit(Request, PfGetEndpointContext(endpoint)->Device, endpoint);
    if (ctx == nullptr) {
        PfCompleteRequestWithoutContext(Request, STATUS_CANCELLED, USBD_STATUS_CANCELED);
        return;
    }
    PfCompleteAsync(ctrl, ctx, STATUS_CANCELLED, USBD_STATUS_CANCELED, 0);
}

/* ------------------------------------------------------------------------ */
/* UDECXUSBENDPOINT callbacks                                               */
/* ------------------------------------------------------------------------ */

_Function_class_(EVT_UDECX_USB_ENDPOINT_START)
void PfEvtEndpointStart(_In_ UDECXUSBENDPOINT UdecxUsbEndpoint)
{
    PF_ENDPOINT_CONTEXT* ep = PfGetEndpointContext(UdecxUsbEndpoint);
    PF_TRACE("endpoint 0x%02X start\n", ep->Address);
    WdfIoQueueStart(ep->Queue);
}

_Function_class_(EVT_WDF_IO_QUEUE_STATE)
void PfEvtEndpointPurgeComplete(_In_ WDFQUEUE Queue, _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Queue);
    UdecxUsbEndpointPurgeComplete(static_cast<UDECXUSBENDPOINT>(Context));
}

/*
 * Stop the queue, fail everything we hold for this endpoint, and tell UdeCx when
 * every request delivered from the queue has been completed.
 *  1. WdfIoQueuePurge first, so that no new request is dispatched to us while we
 *     abort (queued-but-undispatched requests go to EvtIoCanceledOnQueue);
 *  2. PfAbortEndpointRequests completes our queued/in-flight requests (CANCEL
 *     records tell user mode to cancel the remote transfers);
 *  3. the framework calls PfEvtEndpointPurgeComplete once all driver-owned
 *     requests of the queue are completed -> UdecxUsbEndpointPurgeComplete.
 */
_Function_class_(EVT_UDECX_USB_ENDPOINT_PURGE)
void PfEvtEndpointPurge(_In_ UDECXUSBENDPOINT UdecxUsbEndpoint)
{
    PF_ENDPOINT_CONTEXT* ep = PfGetEndpointContext(UdecxUsbEndpoint);
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(PfGetDeviceContext(ep->Device)->Controller);
    PF_TRACE("endpoint 0x%02X purge\n", ep->Address);

    WdfIoQueuePurge(ep->Queue, PfEvtEndpointPurgeComplete, UdecxUsbEndpoint);
    PfAbortEndpointRequests(ctrl, ep->Device, UdecxUsbEndpoint);
}

/*
 * Clear-halt / pipe reset. Forwarded as PFVHCI_REC_ENDPOINT_RESET; the request
 * completes when user mode returns the OP_RESULT (WdfRequestComplete is the only
 * thing UdeCx expects from this request).
 */
_Function_class_(EVT_UDECX_USB_ENDPOINT_RESET)
void PfEvtEndpointReset(_In_ UDECXUSBENDPOINT UdecxUsbEndpoint, _In_ WDFREQUEST Request)
{
    PF_ENDPOINT_CONTEXT* ep = PfGetEndpointContext(UdecxUsbEndpoint);
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(PfGetDeviceContext(ep->Device)->Controller);
    PF_TRACE("endpoint 0x%02X reset\n", ep->Address);

    /* Endpoint == NULL: an operation request must not be cancelled by a purge of the same endpoint. */
    PF_REQUEST_CONTEXT* ctx = PfRequestContextInit(Request, ep->Device, nullptr);
    if (ctx == nullptr) {
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    ctx->RecKind = PfRecKind::EndpointReset;
    ctx->OpEndpoint = ep->Address;
    PfQueueRequestRecord(ctrl, ctx);
}

_Function_class_(EVT_WDF_OBJECT_CONTEXT_CLEANUP)
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfEvtEndpointCleanup(_In_ WDFOBJECT Object)
{
    PF_ENDPOINT_CONTEXT* ep = PfGetEndpointContext(Object);
    if (ep->Device == nullptr) {
        return;
    }
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(PfGetDeviceContext(ep->Device)->Controller);

    WdfSpinLockAcquire(ctrl->Lock);
    if (ep->Linked) {
        RemoveEntryList(&ep->DeviceLink);
        InitializeListHead(&ep->DeviceLink);
        ep->Linked = FALSE;
    }
    WdfSpinLockRelease(ctrl->Lock);
}

} // namespace

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS PfEndpointCreate(_In_ UDECXUSBDEVICE Device, _Inout_ _UDECXUSBENDPOINT_INIT** EndpointInit,
                          _In_opt_ const USB_ENDPOINT_DESCRIPTOR* Descriptor, _In_ ULONG DescriptorBufferLength)
{
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(Device);
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(dev->Controller);

    if (dev->State == PfDeviceState::Gone) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    UINT8 address = 0;
    UINT8 type = PFVHCI_XFER_CONTROL;
    if (Descriptor != nullptr) {
        if (DescriptorBufferLength < sizeof(USB_ENDPOINT_DESCRIPTOR) ||
            Descriptor->bLength < sizeof(USB_ENDPOINT_DESCRIPTOR) || Descriptor->bLength > DescriptorBufferLength) {
            PF_ERROR("endpoint descriptor of %u bytes (bLength %u) rejected\n", DescriptorBufferLength,
                     Descriptor->bLength);
            return STATUS_INVALID_PARAMETER;
        }
        address = Descriptor->bEndpointAddress;
        type = static_cast<UINT8>(Descriptor->bmAttributes & 0x03);
    }

    UdecxUsbEndpointInitSetEndpointAddress(*EndpointInit, address);

    UDECX_USB_ENDPOINT_CALLBACKS callbacks;
    UDECX_USB_ENDPOINT_CALLBACKS_INIT(&callbacks, PfEvtEndpointReset);
    callbacks.EvtUsbEndpointStart = PfEvtEndpointStart;
    callbacks.EvtUsbEndpointPurge = PfEvtEndpointPurge;
    UdecxUsbEndpointInitSetCallbacks(*EndpointInit, &callbacks);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PF_ENDPOINT_CONTEXT);
    attributes.ParentObject = Device;
    attributes.EvtCleanupCallback = PfEvtEndpointCleanup;

    UDECXUSBENDPOINT endpoint = nullptr;
    NTSTATUS status = UdecxUsbEndpointCreate(EndpointInit, &attributes, &endpoint);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("UdecxUsbEndpointCreate(0x%02X) 0x%08X\n", address, status);
        return status;
    }

    PF_ENDPOINT_CONTEXT* ep = PfGetEndpointContext(endpoint);
    ep->Handle = endpoint;
    ep->Device = Device;
    ep->Address = address;
    ep->TransferType = type;
    InitializeListHead(&ep->DeviceLink);

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, type == PFVHCI_XFER_CONTROL ? WdfIoQueueDispatchSequential
                                                                         : WdfIoQueueDispatchParallel);
    /* The controller is virtual and never powers down; power-managed queues would only add stalls. */
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoInternalDeviceControl = PfEvtEndpointIoInternalDeviceControl;
    queueConfig.EvtIoCanceledOnQueue = PfEvtEndpointCanceledOnQueue;

    WDF_OBJECT_ATTRIBUTES queueAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&queueAttributes, PF_QUEUE_CONTEXT);
    queueAttributes.ParentObject = endpoint;

    WDFQUEUE queue = nullptr;
    status = WdfIoQueueCreate(dev->Controller, &queueConfig, &queueAttributes, &queue);
    if (!NT_SUCCESS(status)) {
        /* The endpoint is a child of the device and goes away with it; UdeCx sees the failure. */
        PF_ERROR("WdfIoQueueCreate(endpoint 0x%02X) 0x%08X\n", address, status);
        return status;
    }
    PfGetQueueContext(queue)->Endpoint = endpoint;
    ep->Queue = queue;

    UdecxUsbEndpointSetWdfIoQueue(endpoint, queue);

    WdfSpinLockAcquire(ctrl->Lock);
    InsertTailList(&dev->Endpoints, &ep->DeviceLink);
    ep->Linked = TRUE;
    WdfSpinLockRelease(ctrl->Lock);

    PF_TRACE("device %u endpoint 0x%02X type %u created\n", dev->DeviceHandle, address, type);
    return STATUS_SUCCESS;
}
