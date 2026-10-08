/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * RecordQueue.h - the inverted-call engine between UdeCx requests and user mode.
 *
 *  - Request-backed records (SUBMIT, ENDPOINT_RESET, SELECT_CONFIG, SELECT_INTERFACE,
 *    DEVICE_RESET) are PF_REQUEST_CONTEXTs linked into the owner file's FIFO.
 *  - Informational records (CANCEL, DEVICE_REMOVED) are PF_STANDALONE_RECORDs.
 *  - Pended IOCTL_PFVHCI_FETCH requests wait in a manual queue and are filled by
 *    PfPumpFile() whenever a record or a FETCH arrives.
 *  - Delivered requests wait in the in-flight hash table until IOCTL_PFVHCI_COMPLETE.
 *
 * See RecordQueue.cpp for the cancellation protocol.
 */
#pragma once

#include "Common.h"

/* Creates the completion DPC, the record lookaside list and the FETCH queue. */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfRecordQueueInitialize(_Inout_ PF_CONTROLLER_CONTEXT* Ctrl);

/* Returns the URB of an IOCTL_INTERNAL_USB_SUBMIT_URB request, or NULL. */
_IRQL_requires_max_(DISPATCH_LEVEL)
PURB PfGetRequestUrb(_In_ WDFREQUEST Request);

/*
 * Gets (allocating on first use) and resets the PF_REQUEST_CONTEXT of a request
 * that UdeCx delivered to us. Returns NULL only if the allocation failed.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
PF_REQUEST_CONTEXT* PfRequestContextInit(_In_ WDFREQUEST Request, _In_opt_ UDECXUSBDEVICE Device,
                                         _In_opt_ UDECXUSBENDPOINT Endpoint);

/* Low-resource fallback when no context could be allocated: completes synchronously. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfCompleteRequestWithoutContext(_In_ WDFREQUEST Request, _In_ NTSTATUS NtStatus, _In_ USBD_STATUS UsbdStatus);

/*
 * Completes a request that was never made cancelable (or was already unmarked)
 * through the completion DPC. Required for completions that would otherwise run
 * on the submitter's thread (UdeCx best practice).
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfCompleteAsync(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_REQUEST_CONTEXT* Ctx, _In_ NTSTATUS NtStatus,
                     _In_ USBD_STATUS UsbdStatus, _In_ ULONG Bytes);

/* Completes every PF_REQUEST_CONTEXT (State == Owned) of a list at DISPATCH_LEVEL, now. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfCompleteListNow(_Inout_ PLIST_ENTRY List);

/* Same, but through the completion DPC. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfCompleteListAsync(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PLIST_ENTRY List);

/*
 * Queues a request-backed record. Ctx must have been initialized with
 * PfRequestContextInit and its record fields (RecKind, SUBMIT/op fields) filled.
 * Marks the request cancelable. Fails it (asynchronously) if the device is gone.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfQueueRequestRecord(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_REQUEST_CONTEXT* Ctx);

/*
 * Cancels every queued or in-flight request of Device (restricted to Endpoint if
 * not NULL): STATUS_CANCELLED / USBD_STATUS_CANCELED, plus a CANCEL record for each
 * request user mode had already fetched. Completion goes through the DPC.
 * Requests that are being copied into a FETCH buffer at this moment are caught
 * by the endpoint abort sequence when the FETCH finishes.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfAbortEndpointRequests(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ UDECXUSBDEVICE Device,
                             _In_opt_ UDECXUSBENDPOINT Endpoint);

/*
 * Lock held. Detaches every queued/in-flight request of a device that is being
 * unplugged and appends those this caller now owns to OwnedList (status
 * DEVICE_GONE for URBs, STATUS_SUCCESS for UdeCx operation requests).
 */
_Requires_lock_held_(Ctrl->Lock)
VOID PfCollectDeviceRequestsLocked(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_opt_ PF_FILE_CONTEXT* Owner,
                                   _In_ UDECXUSBDEVICE Device, _Inout_ PLIST_ENTRY OwnedList);

/* Standalone (informational) records. Allocation may fail (returns NULL). */
_IRQL_requires_max_(DISPATCH_LEVEL)
PF_STANDALONE_RECORD* PfAllocStandaloneRecord(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ UINT16 Type,
                                              _In_ ULONG DeviceHandle, _In_ UINT64 Cookie, _In_ UINT64 RequestId);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfFreeStandaloneRecord(_In_ PF_STANDALONE_RECORD* Record);

/* Lock held. Takes ownership of Record on success; on FALSE the caller still owns it. */
_Requires_lock_held_(Ctrl->Lock)
BOOLEAN PfInsertStandaloneLocked(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File,
                                 _Inout_ PF_STANDALONE_RECORD* Record);

/* Lock held. Returns a fresh request id (for records that are not tied to a request). */
_Requires_lock_held_(Ctrl->Lock)
UINT64 PfNextRequestIdLocked(_Inout_ PF_CONTROLLER_CONTEXT* Ctrl);

/*
 * Delivers queued records of FileObject into pended FETCH requests until either
 * runs out. The caller must hold a WDF reference on FileObject (or otherwise
 * guarantee it stays alive). Never called with the lock held.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfPumpFile(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ WDFFILEOBJECT FileObject);

/* IOCTL_PFVHCI_FETCH: pends the request (output size already validated). */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfFetchArrived(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File, _In_ WDFREQUEST Request);

/*
 * IOCTL_PFVHCI_COMPLETE: parses and applies a buffer of COMPLETION / OP_RESULT
 * records. Buffer may be shared with user mode (direct I/O): every field is copied
 * to the stack before it is validated and every write is bounds-checked at use.
 * *Consumed receives the number of bytes successfully parsed.
 */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfProcessCompleteBuffer(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                                          _In_reads_bytes_(Length) const UCHAR* Buffer, _In_ size_t Length,
                                          _Out_ size_t* Consumed);

/* EvtFileCleanup helper: drops informational records and cancels pended FETCHes. */
_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfFileRecordsCleanup(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File);

/* PFVHCI_STATUS_* -> USBD_STATUS / NTSTATUS. */
USBD_STATUS PfMapUsbdStatus(_In_ INT32 PfStatus);
NTSTATUS PfMapNtStatus(_In_ INT32 PfStatus);
