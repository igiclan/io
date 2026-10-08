/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * UsbDevice.h - virtual USB devices (UDECXUSBDEVICE): plug-in, unplug, ports.
 */
#pragma once

#include "Common.h"

/*
 * IOCTL_PFVHCI_PLUGIN. Input is the METHOD_BUFFERED system buffer; it is copied
 * before anything else happens, so the caller may reuse the buffer for output.
 */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfDevicePlugIn(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                                 _In_reads_bytes_(InputLength) const UCHAR* Input, _In_ size_t InputLength,
                                 _Out_ PFVHCI_PLUGIN_RESULT* Result);

/* IOCTL_PFVHCI_UNPLUG. DeviceHandle 0 = every device owned by File. */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfDeviceUnplugByHandle(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                                         _In_ ULONG DeviceHandle);

/* EvtFileCleanup: marks the file closing and unplugs every device it owns. */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfDeviceUnplugAllOwnedBy(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File);

/* EvtDeviceQueryRemove of the controller: unplugs everything, owners get DEVICE_REMOVED. */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfDeviceUnplugAll(_In_ PF_CONTROLLER_CONTEXT* Ctrl);

/*
 * IOCTL_PFVHCI_GET_PORTS. Writes a PFVHCI_PORT_LIST into Output (METHOD_BUFFERED).
 * Returns STATUS_BUFFER_OVERFLOW (with only Count filled, *Written = 8) when the
 * buffer cannot hold every port. Cookies of devices owned by other handles are
 * reported as 0.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS PfDeviceSnapshotPorts(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                               _Out_writes_bytes_(OutputLength) PUCHAR Output, _In_ size_t OutputLength,
                               _Out_ size_t* Written);
