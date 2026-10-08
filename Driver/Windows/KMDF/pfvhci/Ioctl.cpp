/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Ioctl.cpp - user-mode IOCTL dispatch.
 *
 * Buffers (see pfvhci_ioctl.h):
 *   GET_VERSION  METHOD_BUFFERED   out: PFVHCI_VERSION_INFO
 *   PLUGIN       METHOD_BUFFERED   in: PFVHCI_PLUGIN_HEADER + entries, out: PFVHCI_PLUGIN_RESULT
 *   UNPLUG       METHOD_BUFFERED   in: PFVHCI_UNPLUG
 *   FETCH        METHOD_OUT_DIRECT out (MDL): record stream, >= PFVHCI_MIN_FETCH_BUFFER
 *   COMPLETE     METHOD_IN_DIRECT  records in lpInBuffer (buffered copy) OR in lpOutBuffer
 *                                  (MDL, true direct I/O) - exactly one of the two
 *   GET_PORTS    METHOD_BUFFERED   out: PFVHCI_PORT_LIST
 *
 * IOCTL_PFVHCI_* are accepted only on handles opened through the private interface
 * (reference string "PFVHCI"); all other IOCTLs (USB host controller IOCTLs such
 * as IOCTL_USB_GET_ROOT_HUB_NAME used by USBView) go to UdecxWdfDeviceTryHandleUserIoctl.
 */
#include "Ioctl.h"
#include "RecordQueue.h"
#include "UsbDevice.h"

namespace {

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfIoctlGetVersion(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ WDFREQUEST Request)
{
    PAGED_CODE();

    PFVHCI_VERSION_INFO* info = nullptr;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*info), reinterpret_cast<PVOID*>(&info), nullptr);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }
    RtlZeroMemory(info, sizeof(*info));
    info->AbiVersion = PFVHCI_ABI_VERSION;
    info->DriverMajor = PFVHCI_VERSION_MAJOR;
    info->DriverMinor = PFVHCI_VERSION_MINOR;
    info->DriverPatch = PFVHCI_VERSION_PATCH;
    info->DriverBuild = PFVHCI_VERSION_BUILD;
    info->NumUsb2Ports = Ctrl->NumUsb2Ports;
    info->NumUsb3Ports = Ctrl->NumUsb3Ports;
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*info));
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfIoctlPlugIn(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File, _In_ WDFREQUEST Request)
{
    PAGED_CODE();

    PVOID input = nullptr;
    size_t inputLength = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(PFVHCI_PLUGIN_HEADER), &input, &inputLength);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }
    PVOID output = nullptr;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(PFVHCI_PLUGIN_RESULT), &output, nullptr);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    /*
     * METHOD_BUFFERED: input and output share the system buffer. PfDevicePlugIn
     * copies the input before anything else; the result is written afterwards.
     */
    PFVHCI_PLUGIN_RESULT result;
    status = PfDevicePlugIn(Ctrl, File, static_cast<const UCHAR*>(input), inputLength, &result);
    if (!NT_SUCCESS(status)) {
        PF_WARN("PLUGIN failed 0x%08X\n", status);
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }
    RtlCopyMemory(output, &result, sizeof(result));
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(result));
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfIoctlUnplug(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File, _In_ WDFREQUEST Request)
{
    PAGED_CODE();

    PVOID input = nullptr;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(PFVHCI_UNPLUG), &input, nullptr);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }
    PFVHCI_UNPLUG unplug;
    RtlCopyMemory(&unplug, input, sizeof(unplug));
    if (unplug.Reserved != 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_INVALID_PARAMETER, 0);
        return;
    }
    status = PfDeviceUnplugByHandle(Ctrl, File, unplug.DeviceHandle);
    WdfRequestCompleteWithInformation(Request, status, 0);
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfIoctlFetch(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File, _In_ WDFREQUEST Request,
                           size_t OutputBufferLength)
{
    PAGED_CODE();

    if (OutputBufferLength < PFVHCI_MIN_FETCH_BUFFER) {
        WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_TOO_SMALL, 0);
        return;
    }
    if (File->Closing) {
        WdfRequestCompleteWithInformation(Request, STATUS_DELETE_PENDING, 0);
        return;
    }
    PfFetchArrived(Ctrl, File, Request);
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfIoctlComplete(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File, _In_ WDFREQUEST Request,
                              size_t OutputBufferLength, size_t InputBufferLength)
{
    PAGED_CODE();

    /*
     * The ABI header documents the records as the "in" buffer. With METHOD_IN_DIRECT
     * lpInBuffer is *buffered* (copied into nonpaged pool by the I/O manager); only
     * lpOutBuffer is described by an MDL. Both are accepted so that user mode can
     * choose true direct I/O for large IN payloads; supplying both is ambiguous.
     */
    if (InputBufferLength != 0 && OutputBufferLength != 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_INVALID_PARAMETER, 0);
        return;
    }

    PVOID buffer = nullptr;
    size_t length = 0;
    NTSTATUS status;
    if (InputBufferLength != 0) {
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(PFVHCI_RECORD_HEADER), &buffer, &length);
    } else if (OutputBufferLength != 0) {
        /* VERIFY: WdfRequestRetrieveOutputBuffer maps the IN_DIRECT MDL (data flows to the driver). */
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(PFVHCI_RECORD_HEADER), &buffer, &length);
    } else {
        status = STATUS_INVALID_PARAMETER;
    }
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    size_t consumed = 0;
    status = PfProcessCompleteBuffer(Ctrl, File, static_cast<const UCHAR*>(buffer), length, &consumed);
    /* Information = bytes of records accepted (on error: offset of the first bad record). */
    WdfRequestCompleteWithInformation(Request, status, consumed);
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED void PfIoctlGetPorts(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File, _In_ WDFREQUEST Request)
{
    PAGED_CODE();

    PVOID output = nullptr;
    size_t length = 0;
    NTSTATUS status =
        WdfRequestRetrieveOutputBuffer(Request, FIELD_OFFSET(PFVHCI_PORT_LIST, Ports), &output, &length);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }
    size_t written = 0;
    status = PfDeviceSnapshotPorts(Ctrl, File, static_cast<PUCHAR>(output), length, &written);
    WdfRequestCompleteWithInformation(Request, status, written);
}

BOOLEAN PfIsPrivateIoctl(ULONG IoControlCode)
{
    switch (IoControlCode) {
    case IOCTL_PFVHCI_GET_VERSION:
    case IOCTL_PFVHCI_PLUGIN:
    case IOCTL_PFVHCI_UNPLUG:
    case IOCTL_PFVHCI_FETCH:
    case IOCTL_PFVHCI_COMPLETE:
    case IOCTL_PFVHCI_GET_PORTS:
        return TRUE;
    default:
        return FALSE;
    }
}

_Function_class_(EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL)
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                          _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    const WDFDEVICE device = WdfIoQueueGetDevice(Queue);

    if (!PfIsPrivateIoctl(IoControlCode)) {
        /* Standard host controller IOCTLs (USBView, Device Manager) are UdeCx's business. */
        if (!UdecxWdfDeviceTryHandleUserIoctl(device, Request)) {
            WdfRequestCompleteWithInformation(Request, STATUS_INVALID_DEVICE_REQUEST, 0);
        }
        return;
    }

    /*
     * The queue is created with WdfExecutionLevelPassive; this check only guards the
     * paged handlers below against a configuration mistake.
     * VERIFY: WDF accepts WdfExecutionLevelPassive on the default queue of this device.
     */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        WdfRequestCompleteWithInformation(Request, STATUS_INVALID_DEVICE_STATE, 0);
        return;
    }

    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(device);
    const WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    PF_FILE_CONTEXT* file = fileObject != nullptr ? PfGetFileContext(fileObject) : nullptr;
    if (file == nullptr || !file->IsControl) {
        WdfRequestCompleteWithInformation(Request, STATUS_ACCESS_DENIED, 0);
        return;
    }

    switch (IoControlCode) {
    case IOCTL_PFVHCI_GET_VERSION:
        PfIoctlGetVersion(ctrl, Request);
        break;
    case IOCTL_PFVHCI_PLUGIN:
        PfIoctlPlugIn(ctrl, file, Request);
        break;
    case IOCTL_PFVHCI_UNPLUG:
        PfIoctlUnplug(ctrl, file, Request);
        break;
    case IOCTL_PFVHCI_FETCH:
        PfIoctlFetch(ctrl, file, Request, OutputBufferLength);
        break;
    case IOCTL_PFVHCI_COMPLETE:
        PfIoctlComplete(ctrl, file, Request, OutputBufferLength, InputBufferLength);
        break;
    case IOCTL_PFVHCI_GET_PORTS:
        PfIoctlGetPorts(ctrl, file, Request);
        break;
    default:
        WdfRequestCompleteWithInformation(Request, STATUS_INVALID_DEVICE_REQUEST, 0);
        break;
    }
}

} // namespace

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfIoctlCreateDefaultQueue(_Inout_ PF_CONTROLLER_CONTEXT* Ctrl)
{
    PAGED_CODE();

    /*
     * Parallel: FETCH requests are pended (forwarded to the manual queue) and
     * several COMPLETE calls from different user-mode threads must run concurrently.
     * Passive execution level: PLUGIN/UNPLUG call UdeCx APIs that require PASSIVE_LEVEL.
     */
    WDF_IO_QUEUE_CONFIG config;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&config, WdfIoQueueDispatchParallel);
    config.PowerManaged = WdfFalse;
    config.EvtIoDeviceControl = PfEvtIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    const NTSTATUS status = WdfIoQueueCreate(Ctrl->Device, &config, &attributes, &Ctrl->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfIoQueueCreate(default) 0x%08X\n", status);
    }
    return status;
}
