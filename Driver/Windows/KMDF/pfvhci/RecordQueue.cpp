/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * RecordQueue.cpp - inverted-call engine (records, FETCH, COMPLETE, cancellation).
 *
 * ---------------------------------------------------------------------------
 * Cancellation protocol (the subtle part)
 * ---------------------------------------------------------------------------
 * A request-backed record is marked cancelable (WdfRequestMarkCancelableEx) when it
 * is queued and stays cancelable until somebody takes it out of our lists. All
 * transitions happen under PF_CONTROLLER_CONTEXT::Lock:
 *
 *  - A path that wants to complete a request (COMPLETE, unplug, abort) removes it
 *    from its list and calls PfTakeOwnershipLocked(): it stores the status the
 *    request must be completed with, sets State = Owned and calls
 *    WdfRequestUnmarkCancelable (with the lock held; Unmark never waits for the
 *    cancel routine, so this cannot deadlock).
 *      * STATUS_SUCCESS   -> the caller owns the request and completes it.
 *      * STATUS_CANCELLED -> EvtRequestCancel is running or about to run and is
 *        (or will be) spinning on the lock. The caller must not touch the request
 *        again; the cancel routine sees State == Owned and completes it with the
 *        stored status.
 *  - PfEvtRequestCancel acquires the lock and, depending on State:
 *      Queued   -> unlink, complete STATUS_CANCELLED (user mode never saw it);
 *      InFlight -> unlink, complete STATUS_CANCELLED, emit a PFVHCI_REC_CANCEL so
 *                  user mode cancels the remote URB; a late COMPLETION is ignored
 *                  because the RequestId is no longer in the in-flight table;
 *      Fetching -> the FETCH path is copying the request's OUT data right now (lock
 *                  released). Set CancelRoutineRan and return; the FETCH path
 *                  completes the request when it re-acquires the lock. It must not
 *                  call WdfRequestUnmarkCancelable any more (the cancel routine
 *                  already ran);
 *      Owned    -> complete with the stored status (see above).
 *
 * Every completion of a URB happens at DISPATCH_LEVEL and never on the thread that
 * submitted the URB (UdeCx requirement): either through the completion DPC or from
 * our own IOCTL threads after KeRaiseIrql(DISPATCH_LEVEL).
 * ---------------------------------------------------------------------------
 */
#include "RecordQueue.h"

namespace {

/* ------------------------------------------------------------------------ */
/* Entry helpers                                                            */
/* ------------------------------------------------------------------------ */

inline PF_QUEUE_ENTRY* PfEntryFromLink(PLIST_ENTRY Link)
{
    return CONTAINING_RECORD(Link, PF_QUEUE_ENTRY, Link);
}

inline PF_REQUEST_CONTEXT* PfRequestFromEntry(PF_QUEUE_ENTRY* Entry)
{
    return CONTAINING_RECORD(Entry, PF_REQUEST_CONTEXT, Entry);
}

inline PF_STANDALONE_RECORD* PfStandaloneFromEntry(PF_QUEUE_ENTRY* Entry)
{
    return CONTAINING_RECORD(Entry, PF_STANDALONE_RECORD, Entry);
}

inline PF_REQUEST_CONTEXT* PfRequestFromLink(PLIST_ENTRY Link)
{
    return PfRequestFromEntry(PfEntryFromLink(Link));
}

inline ULONG PfBucketOf(UINT64 RequestId)
{
    return static_cast<ULONG>(RequestId & (PF_INFLIGHT_BUCKETS - 1));
}

static_assert((PF_INFLIGHT_BUCKETS & (PF_INFLIGHT_BUCKETS - 1)) == 0, "bucket count must be a power of two");

UINT16 PfRecordTypeOf(PfRecKind Kind)
{
    switch (Kind) {
    case PfRecKind::Submit:          return PFVHCI_REC_SUBMIT;
    case PfRecKind::EndpointReset:   return PFVHCI_REC_ENDPOINT_RESET;
    case PfRecKind::SelectConfig:    return PFVHCI_REC_SELECT_CONFIG;
    case PfRecKind::SelectInterface: return PFVHCI_REC_SELECT_INTERFACE;
    case PfRecKind::DeviceReset:     return PFVHCI_REC_DEVICE_RESET;
    }
    return 0;
}

/*
 * Status used when a request of a device that is being unplugged is failed.
 * UdeCx operation requests (endpoint reset, endpoints configure, device reset)
 * are completed successfully: the device is going away, failing them only makes
 * the class extension retry or log errors during teardown.
 */
void PfGoneStatus(_In_ const PF_REQUEST_CONTEXT* Ctx, _Out_ NTSTATUS* Nt, _Out_ USBD_STATUS* Usbd)
{
    if (Ctx->IsUrb) {
        *Nt = STATUS_DEVICE_NOT_CONNECTED;
        *Usbd = USBD_STATUS_DEVICE_GONE;
    } else {
        *Nt = STATUS_SUCCESS;
        *Usbd = USBD_STATUS_SUCCESS;
    }
}

/* ------------------------------------------------------------------------ */
/* Completion                                                               */
/* ------------------------------------------------------------------------ */

/*
 * Completes a request whose State is Owned. Must run at DISPATCH_LEVEL (UdeCx
 * best practice for URB completion). Ctx must not be used afterwards.
 */
_IRQL_requires_(DISPATCH_LEVEL)
void PfCompleteNow(_Inout_ PF_REQUEST_CONTEXT* Ctx)
{
    const WDFREQUEST request = Ctx->Request;
    const NTSTATUS nt = Ctx->CompleteNt;
    const USBD_STATUS usbd = Ctx->CompleteUsbd;

    if (Ctx->IsUrb) {
        if (Ctx->SetBytes) {
            UdecxUrbSetBytesCompleted(request, Ctx->BytesCompleted);
        }
        if (NT_SUCCESS(nt)) {
            /* USB-level result (success, stall, babble, ...); UdeCx derives the NTSTATUS. */
            UdecxUrbComplete(request, usbd);
        } else {
            /*
             * Cancellation / device gone / malformed request. The URB status is set
             * explicitly so that function drivers that look only at
             * Urb->UrbHeader.Status see the right code.
             * VERIFY: whether UdecxUrbCompleteWithNtStatus overwrites UrbHeader.Status;
             * usbip-win2 reports that completing URBs with an error NTSTATUS after
             * UdecxUsbDevicePlugOutAndDelete can keep UdeCx from deleting the
             * UDECXUSBDEVICE (we complete DEVICE_GONE URBs *before* plug-out, see
             * UsbDevice.cpp, but late cancels can still race with it).
             */
            Ctx->Urb->UrbHeader.Status = usbd;
            UdecxUrbCompleteWithNtStatus(request, nt);
        }
    } else {
        WdfRequestComplete(request, nt);
    }
}

_Function_class_(EVT_WDF_DPC)
_IRQL_requires_(DISPATCH_LEVEL)
void PfEvtCompletionDpc(_In_ WDFDPC Dpc)
{
    auto device = static_cast<WDFDEVICE>(WdfDpcGetParentObject(Dpc));
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(device);

    /* Bounded amount of work per DPC so that a flood of completions cannot hog the CPU. */
    constexpr ULONG kMaxPerDpc = 256;
    ULONG done = 0;

    for (;;) {
        LIST_ENTRY batch;
        InitializeListHead(&batch);

        WdfSpinLockAcquire(ctrl->Lock);
        for (ULONG i = 0; i < 32 && !IsListEmpty(&ctrl->CompletionList); ++i) {
            InsertTailList(&batch, RemoveHeadList(&ctrl->CompletionList));
        }
        const BOOLEAN more = !IsListEmpty(&ctrl->CompletionList);
        WdfSpinLockRelease(ctrl->Lock);

        while (!IsListEmpty(&batch)) {
            PfCompleteNow(PfRequestFromLink(RemoveHeadList(&batch)));
            ++done;
        }

        if (!more) {
            break;
        }
        if (done >= kMaxPerDpc) {
            WdfDpcEnqueue(Dpc);
            break;
        }
    }
}

/* Lock held: appends an Owned request to the DPC list. Caller enqueues the DPC. */
_Requires_lock_held_(Ctrl->Lock)
void PfQueueCompletionLocked(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_REQUEST_CONTEXT* Ctx)
{
    NT_ASSERT(Ctx->State == PfReqState::Owned);
    InsertTailList(&Ctrl->CompletionList, &Ctx->Entry.Link);
}

/*
 * Lock held, Ctx already unlinked from every list. See the protocol description
 * at the top of the file. Returns TRUE if the caller now owns the request.
 */
_Requires_lock_held_(Ctrl->Lock)
BOOLEAN PfTakeOwnershipLocked(_Inout_ PF_REQUEST_CONTEXT* Ctx, _In_ NTSTATUS Nt, _In_ USBD_STATUS Usbd)
{
    Ctx->State = PfReqState::Owned;
    Ctx->CompleteNt = Nt;
    Ctx->CompleteUsbd = Usbd;
    Ctx->BytesCompleted = 0;
    InitializeListHead(&Ctx->Entry.Link);

    if (Ctx->CancelRoutineRan) {
        /* EvtRequestCancel already ran (while Fetching) and left completion to us. */
        return TRUE;
    }

    const NTSTATUS st = WdfRequestUnmarkCancelable(Ctx->Request);
    if (st == STATUS_CANCELLED) {
        return FALSE; /* the pending EvtRequestCancel completes it with the stored status */
    }
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Records                                                                  */
/* ------------------------------------------------------------------------ */

void PfFillOpHeader(_Out_ PFVHCI_ENDPOINT_OP* Op, UINT16 Type, ULONG DeviceHandle, UINT64 Cookie, UINT64 RequestId)
{
    RtlZeroMemory(Op, sizeof(*Op));
    Op->Header.Size = sizeof(PFVHCI_ENDPOINT_OP);
    Op->Header.Type = Type;
    Op->Header.DeviceHandle = DeviceHandle;
    Op->Header.Cookie = Cookie;
    Op->Header.RequestId = RequestId;
}

/* Size of the record an entry turns into. Bounded by ~16.01 MiB (validated at submit). */
ULONG64 PfRecordSize(_In_ PF_QUEUE_ENTRY* Entry)
{
    if (Entry->Kind == PfEntryKind::Standalone) {
        return sizeof(PFVHCI_ENDPOINT_OP);
    }
    const PF_REQUEST_CONTEXT* ctx = PfRequestFromEntry(Entry);
    if (ctx->RecKind != PfRecKind::Submit) {
        return sizeof(PFVHCI_ENDPOINT_OP);
    }
    ULONG64 size = sizeof(PFVHCI_SUBMIT) + static_cast<ULONG64>(ctx->IsoPacketCount) * sizeof(PFVHCI_ISO_PACKET);
    if (!ctx->DirIn) {
        size += ctx->TransferLength;
    }
    return PfAlign8(size);
}

void PfBuildRecordHeader(_In_ PF_QUEUE_ENTRY* Entry, ULONG64 Size, _Out_ PFVHCI_RECORD_HEADER* Header)
{
    RtlZeroMemory(Header, sizeof(*Header));
    Header->Size = static_cast<UINT32>(Size);
    if (Entry->Kind == PfEntryKind::Standalone) {
        *Header = PfStandaloneFromEntry(Entry)->Op.Header;
        return;
    }
    const PF_REQUEST_CONTEXT* ctx = PfRequestFromEntry(Entry);
    Header->Type = PfRecordTypeOf(ctx->RecKind);
    Header->DeviceHandle = ctx->DeviceHandle;
    Header->Cookie = ctx->Cookie;
    Header->RequestId = ctx->RequestId;
}

/*
 * Isochronous packet i of a URB as (offset, length). Lengths are derived from the
 * offsets (USBD_ISO_PACKET_DESCRIPTOR.Length is an output field). Offsets were
 * validated at submit time; this re-checks them so that a misbehaving caller that
 * modifies its URB while it is pending cannot make us read out of bounds.
 */
void PfIsoPacketBounds(_In_ const PF_REQUEST_CONTEXT* Ctx, ULONG Index, _Out_ ULONG* Offset, _Out_ ULONG* Length)
{
    const _URB_ISOCH_TRANSFER& iso = Ctx->Urb->UrbIsochronousTransfer;
    const ULONG offset = iso.IsoPacket[Index].Offset;
    const ULONG next = (Index + 1 < Ctx->IsoPacketCount) ? iso.IsoPacket[Index + 1].Offset : Ctx->TransferLength;
    if (offset <= next && next <= Ctx->TransferLength) {
        *Offset = offset;
        *Length = next - offset;
    } else {
        *Offset = 0;
        *Length = 0;
    }
}

/*
 * Writes one record into a FETCH buffer. Runs WITHOUT the lock: the entry is in
 * State Fetching (request-backed) or private to the caller (standalone), and the
 * URB buffer stays valid because the request cannot complete before the FETCH
 * path finalizes it. Padding is zeroed so no stale kernel data leaks to user mode.
 */
void PfWriteRecord(_Out_writes_bytes_(Size) PUCHAR Dest, ULONG64 Size, _In_ PF_QUEUE_ENTRY* Entry)
{
    if (Entry->Kind == PfEntryKind::Standalone) {
        RtlCopyMemory(Dest, &PfStandaloneFromEntry(Entry)->Op, sizeof(PFVHCI_ENDPOINT_OP));
        return;
    }

    const PF_REQUEST_CONTEXT* ctx = PfRequestFromEntry(Entry);

    if (ctx->RecKind != PfRecKind::Submit) {
        PFVHCI_ENDPOINT_OP op;
        PfFillOpHeader(&op, PfRecordTypeOf(ctx->RecKind), ctx->DeviceHandle, ctx->Cookie, ctx->RequestId);
        op.Endpoint = ctx->OpEndpoint;
        op.ConfigurationValue = ctx->OpConfigurationValue;
        op.InterfaceNumber = ctx->OpInterfaceNumber;
        op.AlternateSetting = ctx->OpAlternateSetting;
        RtlCopyMemory(Dest, &op, sizeof(op));
        return;
    }

    PFVHCI_SUBMIT submit;
    RtlZeroMemory(&submit, sizeof(submit));
    submit.Header.Size = static_cast<UINT32>(Size);
    submit.Header.Type = PFVHCI_REC_SUBMIT;
    submit.Header.DeviceHandle = ctx->DeviceHandle;
    submit.Header.Cookie = ctx->Cookie;
    submit.Header.RequestId = ctx->RequestId;
    submit.Endpoint = ctx->EpAddress;
    submit.TransferType = ctx->XferType;
    submit.TransferFlags = ctx->XferFlags;
    submit.TransferLength = ctx->TransferLength;
    submit.TimeoutMs = ctx->TimeoutMs;
    submit.IsoPacketCount = ctx->IsoPacketCount;
    RtlCopyMemory(submit.Setup, ctx->Setup, sizeof(submit.Setup));
    RtlCopyMemory(Dest, &submit, sizeof(submit));

    ULONG64 offset = sizeof(submit);
    for (ULONG i = 0; i < ctx->IsoPacketCount; ++i) {
        PFVHCI_ISO_PACKET packet;
        ULONG packetOffset = 0;
        ULONG packetLength = 0;
        PfIsoPacketBounds(ctx, i, &packetOffset, &packetLength);
        packet.Offset = packetOffset;
        packet.Length = packetLength;
        RtlCopyMemory(Dest + offset, &packet, sizeof(packet));
        offset += sizeof(packet);
    }

    if (!ctx->DirIn && ctx->TransferLength != 0) {
        RtlCopyMemory(Dest + offset, ctx->Buffer, ctx->TransferLength);
        offset += ctx->TransferLength;
    }

    NT_ASSERT(offset <= Size);
    if (offset < Size) {
        RtlZeroMemory(Dest + offset, static_cast<SIZE_T>(Size - offset));
    }
}

/* ------------------------------------------------------------------------ */
/* Cancel routine                                                           */
/* ------------------------------------------------------------------------ */

_Function_class_(EVT_WDF_REQUEST_CANCEL)
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfEvtRequestCancel(_In_ WDFREQUEST Request)
{
    PF_REQUEST_CONTEXT* ctx = PfGetRequestContext(Request);
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(ctx->Device);
    PF_CONTROLLER_CONTEXT* ctrl = PfGetControllerContext(dev->Controller);

    PF_STANDALONE_RECORD* cancelRecord = nullptr;
    WDFFILEOBJECT pumpFile = nullptr;
    BOOLEAN complete = TRUE;

    WdfSpinLockAcquire(ctrl->Lock);

    switch (ctx->State) {
    case PfReqState::Queued:
        RemoveEntryList(&ctx->Entry.Link);
        ctx->CompleteNt = STATUS_CANCELLED;
        ctx->CompleteUsbd = USBD_STATUS_CANCELED;
        break;

    case PfReqState::InFlight:
        RemoveEntryList(&ctx->Entry.Link);
        ctx->CompleteNt = STATUS_CANCELLED;
        ctx->CompleteUsbd = USBD_STATUS_CANCELED;
        if (dev->Owner != nullptr) {
            PF_FILE_CONTEXT* file = PfGetFileContext(dev->Owner);
            /* Allocating under the spinlock is fine: nonpaged lookaside, DISPATCH_LEVEL safe. */
            cancelRecord = PfAllocStandaloneRecord(ctrl, PFVHCI_REC_CANCEL, ctx->DeviceHandle, ctx->Cookie,
                                                   ctx->RequestId);
            if (cancelRecord != nullptr) {
                cancelRecord->Op.Endpoint = ctx->EpAddress;
                if (PfInsertStandaloneLocked(ctrl, file, cancelRecord)) {
                    cancelRecord = nullptr;
                    pumpFile = dev->Owner;
                    WdfObjectReference(pumpFile);
                }
            } else {
                PF_WARN("cancel: no memory for CANCEL record (request %I64u)\n", ctx->RequestId);
            }
        }
        break;

    case PfReqState::Fetching:
        /* The FETCH path owns the copy; it completes the request when it finalizes. */
        ctx->CancelRoutineRan = TRUE;
        complete = FALSE;
        break;

    case PfReqState::Owned:
        /* Somebody unlinked it but lost the Unmark race; status already stored. */
        break;

    case PfReqState::Idle:
    default:
        ctx->CompleteNt = STATUS_CANCELLED;
        ctx->CompleteUsbd = USBD_STATUS_CANCELED;
        break;
    }

    if (complete) {
        ctx->State = PfReqState::Owned;
        ctx->BytesCompleted = 0;
        PfQueueCompletionLocked(ctrl, ctx);
    }

    WdfSpinLockRelease(ctrl->Lock);

    if (cancelRecord != nullptr) {
        PfFreeStandaloneRecord(cancelRecord);
    }
    if (complete) {
        WdfDpcEnqueue(ctrl->CompletionDpc);
    }
    if (pumpFile != nullptr) {
        PfPumpFile(ctrl, pumpFile);
        WdfObjectDereference(pumpFile);
    }
}

_Function_class_(EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE)
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfEvtFetchCanceledOnQueue(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(Queue);
    /* A FETCH is our own IOCTL, not a URB: completing it here is fine. */
    WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0);
}

/* ------------------------------------------------------------------------ */
/* FETCH                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * Lock held. Decides the fate of a request whose OUT data was just copied into a
 * FETCH buffer. Appends requests that must be completed to Complete.
 */
_Requires_lock_held_(Ctrl->Lock)
void PfFinalizeFetchedLocked(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File,
                             _Inout_ PF_REQUEST_CONTEXT* Ctx, _Inout_ PLIST_ENTRY Complete)
{
    NT_ASSERT(Ctx->State == PfReqState::Fetching);
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(Ctx->Device);

    BOOLEAN emitCancel = FALSE;
    BOOLEAN owned = FALSE;

    if (Ctx->CancelRoutineRan) {
        /* Cancelled by the OS while being copied: user mode now holds a SUBMIT for it. */
        owned = PfTakeOwnershipLocked(Ctx, STATUS_CANCELLED, USBD_STATUS_CANCELED);
        NT_ASSERT(owned);
        emitCancel = TRUE;
    } else if (dev->State == PfDeviceState::Gone) {
        NTSTATUS nt;
        USBD_STATUS usbd;
        PfGoneStatus(Ctx, &nt, &usbd);
        owned = PfTakeOwnershipLocked(Ctx, nt, usbd);
    } else if (Ctx->Endpoint != nullptr &&
               PfGetEndpointContext(Ctx->Endpoint)->AbortSequence != Ctx->AbortSequence) {
        /* The endpoint was purged / aborted while we were copying. */
        owned = PfTakeOwnershipLocked(Ctx, STATUS_CANCELLED, USBD_STATUS_CANCELED);
        emitCancel = TRUE;
    } else {
        Ctx->State = PfReqState::InFlight;
        InsertTailList(&Ctrl->InFlight[PfBucketOf(Ctx->RequestId)], &Ctx->Entry.Link);
        return;
    }

    if (emitCancel && !File->Closing) {
        PF_STANDALONE_RECORD* rec = PfAllocStandaloneRecord(Ctrl, PFVHCI_REC_CANCEL, Ctx->DeviceHandle, Ctx->Cookie,
                                                            Ctx->RequestId);
        if (rec != nullptr) {
            rec->Op.Endpoint = Ctx->EpAddress;
            if (!PfInsertStandaloneLocked(Ctrl, File, rec)) {
                PfFreeStandaloneRecord(rec);
            }
        }
    }

    if (owned) {
        InsertTailList(Complete, &Ctx->Entry.Link);
    }
}

/*
 * Fills one FETCH request. Called by the (single) active pump of the file, without
 * the lock. Never re-enters PfPumpFile.
 */
void PfFillFetch(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File, _In_ WDFREQUEST Fetch)
{
    PVOID buffer = nullptr;
    size_t length = 0;
    NTSTATUS st = WdfRequestRetrieveOutputBuffer(Fetch, sizeof(PFVHCI_RECORD_HEADER), &buffer, &length);
    if (!NT_SUCCESS(st)) {
        PF_ERROR("FETCH: WdfRequestRetrieveOutputBuffer 0x%08X\n", st);
        WdfRequestCompleteWithInformation(Fetch, st, 0);
        return;
    }
    if (length > MAXULONG) {
        length = MAXULONG; /* Information is reported as ULONG_PTR but records use 32-bit sizes */
    }

    LIST_ENTRY batch;
    InitializeListHead(&batch);
    ULONG64 used = 0;
    ULONG count = 0;
    BOOLEAN overflow = FALSE;
    PFVHCI_RECORD_HEADER overflowHeader;
    RtlZeroMemory(&overflowHeader, sizeof(overflowHeader));

    WdfSpinLockAcquire(Ctrl->Lock);
    while (!IsListEmpty(&File->Records)) {
        PLIST_ENTRY link = File->Records.Flink;
        PF_QUEUE_ENTRY* entry = PfEntryFromLink(link);
        const ULONG64 size = PfRecordSize(entry);

        if (used + size > length) {
            if (count == 0) {
                /* The first record does not fit an empty buffer: report the size needed. */
                overflow = TRUE;
                PfBuildRecordHeader(entry, size, &overflowHeader);
            }
            break;
        }

        RemoveEntryList(link);
        if (entry->Kind == PfEntryKind::Standalone) {
            NT_ASSERT(File->StandaloneCount > 0);
            --File->StandaloneCount;
        } else {
            PfRequestFromEntry(entry)->State = PfReqState::Fetching;
        }
        InsertTailList(&batch, link);
        used += size;
        ++count;
    }
    WdfSpinLockRelease(Ctrl->Lock);

    if (overflow) {
        /* Header only; the record stays queued for a retry with a bigger buffer. */
        RtlCopyMemory(buffer, &overflowHeader, sizeof(overflowHeader));
        WdfRequestCompleteWithInformation(Fetch, STATUS_BUFFER_OVERFLOW, sizeof(PFVHCI_RECORD_HEADER));
        return;
    }

    if (count == 0) {
        /* Records disappeared (cancelled) between the pump's check and now. */
        st = WdfRequestRequeue(Fetch);
        if (!NT_SUCCESS(st)) {
            WdfRequestCompleteWithInformation(Fetch, st, 0);
        }
        return;
    }

    /* Copy without the lock. */
    PUCHAR out = static_cast<PUCHAR>(buffer);
    ULONG64 offset = 0;
    for (PLIST_ENTRY link = batch.Flink; link != &batch; link = link->Flink) {
        PF_QUEUE_ENTRY* entry = PfEntryFromLink(link);
        const ULONG64 size = PfRecordSize(entry);
        PfWriteRecord(out + offset, size, entry);
        offset += size;
    }
    NT_ASSERT(offset == used);

    /* Finalize: move delivered requests to the in-flight table. */
    LIST_ENTRY complete;
    LIST_ENTRY freeRecords;
    InitializeListHead(&complete);
    InitializeListHead(&freeRecords);

    WdfSpinLockAcquire(Ctrl->Lock);
    while (!IsListEmpty(&batch)) {
        PLIST_ENTRY link = RemoveHeadList(&batch);
        PF_QUEUE_ENTRY* entry = PfEntryFromLink(link);
        if (entry->Kind == PfEntryKind::Standalone) {
            InsertTailList(&freeRecords, link);
        } else {
            PfFinalizeFetchedLocked(Ctrl, File, PfRequestFromEntry(entry), &complete);
        }
    }
    WdfSpinLockRelease(Ctrl->Lock);

    while (!IsListEmpty(&freeRecords)) {
        PfFreeStandaloneRecord(PfStandaloneFromEntry(PfEntryFromLink(RemoveHeadList(&freeRecords))));
    }
    if (!IsListEmpty(&complete)) {
        PfCompleteListAsync(Ctrl, &complete);
    }

    WdfRequestCompleteWithInformation(Fetch, STATUS_SUCCESS, static_cast<ULONG_PTR>(used));
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void PfCancelFetches(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ WDFFILEOBJECT FileObject)
{
    WDFREQUEST request;
    while (NT_SUCCESS(WdfIoQueueRetrieveRequestByFileObject(Ctrl->FetchQueue, FileObject, &request))) {
        WdfRequestCompleteWithInformation(request, STATUS_CANCELLED, 0);
    }
}

/* ------------------------------------------------------------------------ */
/* COMPLETE                                                                 */
/* ------------------------------------------------------------------------ */

/* URB-level result of a PFVHCI_STATUS_* (see PfCompleteNow for how it is applied). */
void PfUrbResult(INT32 PfStatus, _Out_ NTSTATUS* Nt, _Out_ USBD_STATUS* Usbd)
{
    *Usbd = PfMapUsbdStatus(PfStatus);
    switch (PfStatus) {
    case PFVHCI_STATUS_CANCELLED:
        *Nt = STATUS_CANCELLED;
        break;
    case PFVHCI_STATUS_DEVICE_GONE:
    case PFVHCI_STATUS_NO_DEVICE:
        *Nt = STATUS_DEVICE_NOT_CONNECTED;
        break;
    default:
        *Nt = STATUS_SUCCESS; /* success or a USB error expressed through USBD_STATUS */
        break;
    }
}

/*
 * Looks up a delivered request by the header of a COMPLETION/OP_RESULT and takes
 * ownership of it. Returns NULL for unknown ids (late completion after a cancel),
 * for records that do not belong to this file object, and when the request is
 * being cancelled concurrently (it is then completed as cancelled).
 */
PF_REQUEST_CONTEXT* PfTakeInFlight(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                                   _In_ const PFVHCI_RECORD_HEADER* Header, BOOLEAN IsCompletion)
{
    PF_REQUEST_CONTEXT* found = nullptr;
    const char* reason = nullptr;

    WdfSpinLockAcquire(Ctrl->Lock);

    PLIST_ENTRY head = &Ctrl->InFlight[PfBucketOf(Header->RequestId)];
    for (PLIST_ENTRY link = head->Flink; link != head; link = link->Flink) {
        PF_REQUEST_CONTEXT* ctx = PfRequestFromLink(link);
        if (ctx->RequestId == Header->RequestId) {
            found = ctx;
            break;
        }
    }

    if (found == nullptr) {
        reason = "unknown or cancelled request id";
    } else if (PfGetDeviceContext(found->Device)->Owner != File->FileObject ||
               found->DeviceHandle != Header->DeviceHandle) {
        reason = "request belongs to another handle/device";
        found = nullptr;
    } else if ((found->RecKind == PfRecKind::Submit) != (IsCompletion != FALSE)) {
        reason = "record type does not match the request";
        found = nullptr;
    } else {
        RemoveEntryList(&found->Entry.Link);
        /* If we lose the race with EvtRequestCancel, it completes the request as cancelled. */
        if (!PfTakeOwnershipLocked(found, STATUS_CANCELLED, USBD_STATUS_CANCELED)) {
            reason = "request is being cancelled";
            found = nullptr;
        }
    }

    WdfSpinLockRelease(Ctrl->Lock);

    if (reason != nullptr) {
        PF_TRACE("COMPLETE: ignoring record for request %I64u: %s\n", Header->RequestId, reason);
    }
    return found;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void PfCompleteOwnedNow(_Inout_ PF_REQUEST_CONTEXT* Ctx)
{
    KIRQL oldIrql;
    KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);
    PfCompleteNow(Ctx);
    KeLowerIrql(oldIrql);
}

/*
 * Applies a COMPLETION record (Size already validated against the buffer and
 * >= sizeof(PFVHCI_COMPLETION)). Record points into a buffer that user mode may
 * still be modifying: fixed fields are read once into locals and every copy into
 * the URB buffer is bounded by values the driver owns.
 */
void PfApplyCompletion(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                       _In_reads_bytes_(Size) const UCHAR* Record, ULONG Size)
{
    PFVHCI_COMPLETION c;
    RtlCopyMemory(&c, Record, sizeof(c));

    PF_REQUEST_CONTEXT* ctx = PfTakeInFlight(Ctrl, File, &c.Header, TRUE);
    if (ctx == nullptr) {
        return;
    }

    const BOOLEAN iso = ctx->XferType == PFVHCI_XFER_ISOCHRONOUS;
    BOOLEAN valid = c.IsoPacketCount <= PFVHCI_MAX_ISO_PACKETS && c.ActualLength <= PFVHCI_MAX_TRANSFER_BYTES;
    if (valid) {
        const ULONG64 expected = PfAlign8(sizeof(PFVHCI_COMPLETION) +
                                          static_cast<ULONG64>(c.IsoPacketCount) * sizeof(PFVHCI_ISO_RESULT) +
                                          (ctx->DirIn ? c.ActualLength : 0));
        valid = expected == Size;
    }
    if (valid) {
        valid = iso ? (c.IsoPacketCount == 0 || c.IsoPacketCount == ctx->IsoPacketCount) : (c.IsoPacketCount == 0);
    }

    NTSTATUS nt = STATUS_INVALID_PARAMETER;
    USBD_STATUS usbd = USBD_STATUS_INVALID_PARAMETER;
    ULONG bytes = 0;

    if (valid) {
        PfUrbResult(c.Status, &nt, &usbd);
        const UCHAR* results = Record + sizeof(PFVHCI_COMPLETION);
        const UCHAR* data = results + static_cast<SIZE_T>(c.IsoPacketCount) * sizeof(PFVHCI_ISO_RESULT);

        if (!iso) {
            if (ctx->DirIn) {
                bytes = c.ActualLength <= ctx->TransferLength ? c.ActualLength : ctx->TransferLength;
                if (bytes != 0) {
                    RtlCopyMemory(ctx->Buffer, data, bytes);
                }
                if (c.ActualLength > ctx->TransferLength && NT_SUCCESS(nt) && USBD_SUCCESS(usbd)) {
                    /* Remote returned more than the URB can hold. */
                    usbd = USBD_STATUS_BABBLE_DETECTED;
                }
            } else {
                bytes = c.ActualLength <= ctx->TransferLength ? c.ActualLength : ctx->TransferLength;
            }
        } else {
            _URB_ISOCH_TRANSFER& urbIso = ctx->Urb->UrbIsochronousTransfer;
            if (c.IsoPacketCount == 0) {
                /*
                 * No per-packet results: only allowed without IN data (typically the
                 * whole transfer failed). Every packet inherits the overall status.
                 */
                if (ctx->DirIn && c.ActualLength != 0) {
                    valid = FALSE;
                } else {
                    const BOOLEAN ok = NT_SUCCESS(nt) && USBD_SUCCESS(usbd);
                    for (ULONG i = 0; i < ctx->IsoPacketCount; ++i) {
                        if (ctx->DirIn) {
                            urbIso.IsoPacket[i].Length = 0;
                        }
                        urbIso.IsoPacket[i].Status = usbd;
                    }
                    urbIso.ErrorCount = ok ? 0 : ctx->IsoPacketCount;
                    if (!ctx->DirIn) {
                        bytes = c.ActualLength <= ctx->TransferLength ? c.ActualLength : ctx->TransferLength;
                    }
                }
            } else {
                /* IN data is packed: packet i contributes exactly Iso[i].ActualLength bytes. */
                ULONG64 consumed = 0;
                ULONG64 total = 0;
                for (ULONG i = 0; i < ctx->IsoPacketCount && valid; ++i) {
                    PFVHCI_ISO_RESULT r;
                    RtlCopyMemory(&r, results + static_cast<SIZE_T>(i) * sizeof(r), sizeof(r));
                    ULONG packetOffset = 0;
                    ULONG packetCapacity = 0;
                    PfIsoPacketBounds(ctx, i, &packetOffset, &packetCapacity);

                    if (r.ActualLength > packetCapacity) {
                        valid = FALSE;
                        break;
                    }
                    if (ctx->DirIn) {
                        if (r.ActualLength > c.ActualLength - consumed) {
                            valid = FALSE;
                            break;
                        }
                        if (r.ActualLength != 0) {
                            RtlCopyMemory(ctx->Buffer + packetOffset, data + consumed, r.ActualLength);
                        }
                        consumed += r.ActualLength;
                        urbIso.IsoPacket[i].Length = r.ActualLength;
                    }
                    urbIso.IsoPacket[i].Status = PfMapUsbdStatus(r.Status);
                    total += r.ActualLength;
                }
                if (valid && ctx->DirIn && consumed != c.ActualLength) {
                    valid = FALSE;
                }
                if (valid) {
                    urbIso.ErrorCount = c.IsoErrorCount <= ctx->IsoPacketCount ? c.IsoErrorCount : ctx->IsoPacketCount;
                    bytes = static_cast<ULONG>(total <= ctx->TransferLength ? total : ctx->TransferLength);
                }
            }
        }
    }

    if (!valid) {
        PF_WARN("COMPLETE: malformed COMPLETION for request %I64u (size %u)\n", ctx->RequestId, Size);
        nt = STATUS_INVALID_PARAMETER;
        usbd = USBD_STATUS_INVALID_PARAMETER;
        bytes = 0;
    }

    ctx->CompleteNt = nt;
    ctx->CompleteUsbd = usbd;
    ctx->BytesCompleted = bytes;
    PF_URB_TRACE("complete req %I64u nt 0x%08X usbd 0x%08X bytes %u\n", ctx->RequestId, nt, usbd, bytes);
    PfCompleteOwnedNow(ctx);
}

void PfApplyOpResult(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File, _In_reads_bytes_(sizeof(PFVHCI_OP_RESULT)) const UCHAR* Record)
{
    PFVHCI_OP_RESULT r;
    RtlCopyMemory(&r, Record, sizeof(r));

    PF_REQUEST_CONTEXT* ctx = PfTakeInFlight(Ctrl, File, &r.Header, FALSE);
    if (ctx == nullptr) {
        return;
    }

    if (ctx->IsUrb) {
        /* e.g. URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL turned into ENDPOINT_RESET */
        PfUrbResult(r.Status, &ctx->CompleteNt, &ctx->CompleteUsbd);
    } else {
        ctx->CompleteNt = PfMapNtStatus(r.Status);
        ctx->CompleteUsbd = PfMapUsbdStatus(r.Status);
    }
    ctx->BytesCompleted = 0;
    PfCompleteOwnedNow(ctx);
}

/*
 * Nonpaged: takes the spinlock. Moves every remaining record of a closing file out
 * of its list. All devices of the file were unplugged before this runs, so request-
 * backed entries are not expected; they are failed defensively.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
void PfDetachAllFileRecords(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File,
                            _Inout_ PLIST_ENTRY Records, _Inout_ PLIST_ENTRY Owned)
{
    WdfSpinLockAcquire(Ctrl->Lock);
    File->Closing = TRUE;
    while (!IsListEmpty(&File->Records)) {
        PLIST_ENTRY link = RemoveHeadList(&File->Records);
        PF_QUEUE_ENTRY* entry = PfEntryFromLink(link);
        if (entry->Kind == PfEntryKind::Standalone) {
            InsertTailList(Records, link);
        } else {
            PF_REQUEST_CONTEXT* ctx = PfRequestFromEntry(entry);
            NTSTATUS nt;
            USBD_STATUS usbd;
            PfGoneStatus(ctx, &nt, &usbd);
            if (PfTakeOwnershipLocked(ctx, nt, usbd)) {
                InsertTailList(Owned, &ctx->Entry.Link);
            }
        }
    }
    File->StandaloneCount = 0;
    WdfSpinLockRelease(Ctrl->Lock);
}

} // namespace

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

USBD_STATUS PfMapUsbdStatus(_In_ INT32 PfStatus)
{
    switch (PfStatus) {
    case PFVHCI_STATUS_SUCCESS:       return USBD_STATUS_SUCCESS;
    case PFVHCI_STATUS_STALL:         return USBD_STATUS_STALL_PID;
    case PFVHCI_STATUS_TIMEOUT:       return USBD_STATUS_TIMEOUT;
    case PFVHCI_STATUS_CANCELLED:     return USBD_STATUS_CANCELED;
    case PFVHCI_STATUS_OVERFLOW:      return USBD_STATUS_BABBLE_DETECTED;
    case PFVHCI_STATUS_SHORT_PACKET:  return USBD_STATUS_ERROR_SHORT_TRANSFER;
    case PFVHCI_STATUS_DEVICE_GONE:
    case PFVHCI_STATUS_NO_DEVICE:     return USBD_STATUS_DEVICE_GONE;
    case PFVHCI_STATUS_INVALID:       return USBD_STATUS_INVALID_PARAMETER;
    case PFVHCI_STATUS_NOT_SUPPORTED: return USBD_STATUS_NOT_SUPPORTED;
    case PFVHCI_STATUS_IO_ERROR:      return USBD_STATUS_DEV_NOT_RESPONDING;
    default:                          return USBD_STATUS_REQUEST_FAILED;
    }
}

NTSTATUS PfMapNtStatus(_In_ INT32 PfStatus)
{
    switch (PfStatus) {
    case PFVHCI_STATUS_SUCCESS:       return STATUS_SUCCESS;
    case PFVHCI_STATUS_TIMEOUT:       return STATUS_IO_TIMEOUT;
    case PFVHCI_STATUS_CANCELLED:     return STATUS_CANCELLED;
    case PFVHCI_STATUS_OVERFLOW:      return STATUS_DATA_OVERRUN;
    case PFVHCI_STATUS_DEVICE_GONE:
    case PFVHCI_STATUS_NO_DEVICE:     return STATUS_DEVICE_NOT_CONNECTED;
    case PFVHCI_STATUS_INVALID:       return STATUS_INVALID_PARAMETER;
    case PFVHCI_STATUS_NOT_SUPPORTED: return STATUS_NOT_SUPPORTED;
    case PFVHCI_STATUS_IO_ERROR:      return STATUS_IO_DEVICE_ERROR;
    case PFVHCI_STATUS_STALL:
    case PFVHCI_STATUS_SHORT_PACKET:
    default:                          return STATUS_UNSUCCESSFUL;
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfRecordQueueInitialize(_Inout_ PF_CONTROLLER_CONTEXT* Ctrl)
{
    PAGED_CODE();

    InitializeListHead(&Ctrl->CompletionList);
    for (ULONG i = 0; i < PF_INFLIGHT_BUCKETS; ++i) {
        InitializeListHead(&Ctrl->InFlight[i]);
    }
    Ctrl->NextRequestId = 0;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctrl->Device;

    WDF_DPC_CONFIG dpcConfig;
    WDF_DPC_CONFIG_INIT(&dpcConfig, PfEvtCompletionDpc);
    dpcConfig.AutomaticSerialization = FALSE;

    NTSTATUS status = WdfDpcCreate(&dpcConfig, &attributes, &Ctrl->CompletionDpc);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfDpcCreate 0x%08X\n", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctrl->Device;
    status = WdfLookasideListCreate(&attributes, sizeof(PF_STANDALONE_RECORD), NonPagedPoolNx,
                                    WDF_NO_OBJECT_ATTRIBUTES, PF_POOL_TAG, &Ctrl->RecordLookaside);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfLookasideListCreate 0x%08X\n", status);
        return status;
    }

    /* Pended FETCH requests. Not power managed: the controller is virtual. */
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoCanceledOnQueue = PfEvtFetchCanceledOnQueue;

    status = WdfIoQueueCreate(Ctrl->Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Ctrl->FetchQueue);
    if (!NT_SUCCESS(status)) {
        PF_ERROR("WdfIoQueueCreate(fetch) 0x%08X\n", status);
        return status;
    }
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
PURB PfGetRequestUrb(_In_ WDFREQUEST Request)
{
    PIRP irp = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL &&
        stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        return static_cast<PURB>(stack->Parameters.Others.Argument1); /* = URB_FROM_IRP */
    }
    return nullptr;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
PF_REQUEST_CONTEXT* PfRequestContextInit(_In_ WDFREQUEST Request, _In_opt_ UDECXUSBDEVICE Device,
                                         _In_opt_ UDECXUSBENDPOINT Endpoint)
{
    /*
     * Requests on endpoint queues are created by UdeCx, so the device-wide request
     * attributes do not apply to them; the context is attached on first use. UdeCx
     * may reuse a request object after completion, hence the reset.
     */
    PF_REQUEST_CONTEXT* ctx = PfGetRequestContext(Request);
    if (ctx == nullptr) {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PF_REQUEST_CONTEXT);
        PVOID memory = nullptr;
        const NTSTATUS status = WdfObjectAllocateContext(Request, &attributes, &memory);
        if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_EXISTS) {
            PF_ERROR("WdfObjectAllocateContext(request) 0x%08X\n", status);
            return nullptr;
        }
        ctx = PfGetRequestContext(Request);
        if (ctx == nullptr) {
            return nullptr;
        }
    }

    RtlZeroMemory(ctx, sizeof(*ctx));
    InitializeListHead(&ctx->Entry.Link);
    ctx->Entry.Kind = PfEntryKind::Request;
    ctx->Request = Request;
    ctx->Device = Device;
    ctx->Endpoint = Endpoint;
    ctx->Urb = PfGetRequestUrb(Request);
    ctx->IsUrb = ctx->Urb != nullptr;
    ctx->State = PfReqState::Idle;
    ctx->CompleteNt = STATUS_SUCCESS;
    ctx->CompleteUsbd = USBD_STATUS_SUCCESS;
    return ctx;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfCompleteRequestWithoutContext(_In_ WDFREQUEST Request, _In_ NTSTATUS NtStatus, _In_ USBD_STATUS UsbdStatus)
{
    PURB urb = PfGetRequestUrb(Request);
    if (urb != nullptr) {
        urb->UrbHeader.Status = UsbdStatus;
        UdecxUrbCompleteWithNtStatus(Request, NtStatus);
    } else {
        WdfRequestComplete(Request, NtStatus);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfCompleteAsync(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_REQUEST_CONTEXT* Ctx, _In_ NTSTATUS NtStatus,
                     _In_ USBD_STATUS UsbdStatus, _In_ ULONG Bytes)
{
    Ctx->CompleteNt = NtStatus;
    Ctx->CompleteUsbd = UsbdStatus;
    Ctx->BytesCompleted = Bytes;

    WdfSpinLockAcquire(Ctrl->Lock);
    Ctx->State = PfReqState::Owned;
    PfQueueCompletionLocked(Ctrl, Ctx);
    WdfSpinLockRelease(Ctrl->Lock);

    WdfDpcEnqueue(Ctrl->CompletionDpc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfCompleteListNow(_Inout_ PLIST_ENTRY List)
{
    if (IsListEmpty(List)) {
        return;
    }
    KIRQL oldIrql;
    KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);
    while (!IsListEmpty(List)) {
        PfCompleteNow(PfRequestFromLink(RemoveHeadList(List)));
    }
    KeLowerIrql(oldIrql);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfCompleteListAsync(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PLIST_ENTRY List)
{
    if (IsListEmpty(List)) {
        return;
    }
    WdfSpinLockAcquire(Ctrl->Lock);
    while (!IsListEmpty(List)) {
        PLIST_ENTRY link = RemoveHeadList(List);
        NT_ASSERT(PfRequestFromLink(link)->State == PfReqState::Owned);
        InsertTailList(&Ctrl->CompletionList, link);
    }
    WdfSpinLockRelease(Ctrl->Lock);
    WdfDpcEnqueue(Ctrl->CompletionDpc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
PF_STANDALONE_RECORD* PfAllocStandaloneRecord(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ UINT16 Type,
                                              _In_ ULONG DeviceHandle, _In_ UINT64 Cookie, _In_ UINT64 RequestId)
{
    WDFMEMORY memory;
    if (!NT_SUCCESS(WdfMemoryCreateFromLookaside(Ctrl->RecordLookaside, &memory))) {
        return nullptr;
    }
    size_t size = 0;
    auto record = static_cast<PF_STANDALONE_RECORD*>(WdfMemoryGetBuffer(memory, &size));
    if (record == nullptr || size < sizeof(*record)) {
        WdfObjectDelete(memory);
        return nullptr;
    }
    RtlZeroMemory(record, sizeof(*record));
    InitializeListHead(&record->Entry.Link);
    record->Entry.Kind = PfEntryKind::Standalone;
    record->Memory = memory;
    PfFillOpHeader(&record->Op, Type, DeviceHandle, Cookie, RequestId);
    return record;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfFreeStandaloneRecord(_In_ PF_STANDALONE_RECORD* Record)
{
    WdfObjectDelete(Record->Memory);
}

_Requires_lock_held_(Ctrl->Lock)
BOOLEAN PfInsertStandaloneLocked(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File,
                                 _Inout_ PF_STANDALONE_RECORD* Record)
{
    UNREFERENCED_PARAMETER(Ctrl);
    if (File->Closing) {
        return FALSE;
    }
    if (File->StandaloneCount >= PF_MAX_STANDALONE_RECORDS_PER_FILE) {
        PF_WARN("dropping informational record type %u: user mode is not fetching\n", Record->Op.Header.Type);
        return FALSE;
    }
    InsertTailList(&File->Records, &Record->Entry.Link);
    ++File->StandaloneCount;
    return TRUE;
}

_Requires_lock_held_(Ctrl->Lock)
UINT64 PfNextRequestIdLocked(_Inout_ PF_CONTROLLER_CONTEXT* Ctrl)
{
    return ++Ctrl->NextRequestId; /* starts at 1; 2^64 ids never wrap in practice */
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfQueueRequestRecord(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_REQUEST_CONTEXT* Ctx)
{
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(Ctx->Device);
    WDFFILEOBJECT owner = nullptr;
    BOOLEAN gone = FALSE;
    BOOLEAN cancelled = FALSE;

    WdfSpinLockAcquire(Ctrl->Lock);

    if (dev->State != PfDeviceState::PluggedIn || dev->Owner == nullptr ||
        PfGetFileContext(dev->Owner)->Closing) {
        gone = TRUE;
    } else {
        Ctx->DeviceHandle = dev->DeviceHandle;
        Ctx->Cookie = dev->Cookie;
        Ctx->RequestId = PfNextRequestIdLocked(Ctrl);
    }

    if (!gone && Ctx->Endpoint != nullptr &&
        PfGetEndpointContext(Ctx->Endpoint)->AbortSequence != Ctx->AbortSequence) {
        /*
         * The endpoint was purged/aborted while this URB was being translated
         * (Ctx->AbortSequence was captured when the queue dispatched it). Queuing it
         * now would let it escape the abort sweep and stall the purge.
         */
        cancelled = TRUE;
    } else if (!gone) {
        /*
         * MarkCancelableEx (unlike MarkCancelable) never calls the cancel routine
         * synchronously, so it is safe under our spinlock.
         */
        const NTSTATUS st = WdfRequestMarkCancelableEx(Ctx->Request, PfEvtRequestCancel);
        if (!NT_SUCCESS(st)) {
            cancelled = TRUE;
        } else {
            PF_FILE_CONTEXT* file = PfGetFileContext(dev->Owner);
            Ctx->State = PfReqState::Queued;
            InsertTailList(&file->Records, &Ctx->Entry.Link);
            owner = dev->Owner;
            WdfObjectReference(owner);
        }
    }

    WdfSpinLockRelease(Ctrl->Lock);

    if (gone) {
        NTSTATUS nt;
        USBD_STATUS usbd;
        PfGoneStatus(Ctx, &nt, &usbd);
        PfCompleteAsync(Ctrl, Ctx, nt, usbd, 0);
        return;
    }
    if (cancelled) {
        PfCompleteAsync(Ctrl, Ctx, STATUS_CANCELLED, USBD_STATUS_CANCELED, 0);
        return;
    }

    PfPumpFile(Ctrl, owner);
    WdfObjectDereference(owner);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfAbortEndpointRequests(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ UDECXUSBDEVICE Device,
                             _In_opt_ UDECXUSBENDPOINT Endpoint)
{
    PF_DEVICE_CONTEXT* dev = PfGetDeviceContext(Device);
    LIST_ENTRY owned;
    InitializeListHead(&owned);
    WDFFILEOBJECT pumpFile = nullptr;
    ULONG cancelRecordsLost = 0;

    WdfSpinLockAcquire(Ctrl->Lock);

    if (Endpoint != nullptr) {
        ++PfGetEndpointContext(Endpoint)->AbortSequence;
    } else {
        for (PLIST_ENTRY link = dev->Endpoints.Flink; link != &dev->Endpoints; link = link->Flink) {
            ++CONTAINING_RECORD(link, PF_ENDPOINT_CONTEXT, DeviceLink)->AbortSequence;
        }
    }

    PF_FILE_CONTEXT* file = dev->Owner != nullptr ? PfGetFileContext(dev->Owner) : nullptr;

    /* 1. Not yet fetched: just cancel, user mode never heard of them. */
    if (file != nullptr) {
        PLIST_ENTRY link = file->Records.Flink;
        while (link != &file->Records) {
            PLIST_ENTRY next = link->Flink;
            PF_QUEUE_ENTRY* entry = PfEntryFromLink(link);
            if (entry->Kind == PfEntryKind::Request) {
                PF_REQUEST_CONTEXT* ctx = PfRequestFromEntry(entry);
                if (ctx->Device == Device && (Endpoint == nullptr || ctx->Endpoint == Endpoint)) {
                    RemoveEntryList(link);
                    if (PfTakeOwnershipLocked(ctx, STATUS_CANCELLED, USBD_STATUS_CANCELED)) {
                        InsertTailList(&owned, &ctx->Entry.Link);
                    }
                }
            }
            link = next;
        }
    }

    /* 2. Delivered: cancel locally and tell user mode to cancel the remote transfer. */
    for (ULONG b = 0; b < PF_INFLIGHT_BUCKETS; ++b) {
        PLIST_ENTRY head = &Ctrl->InFlight[b];
        PLIST_ENTRY link = head->Flink;
        while (link != head) {
            PLIST_ENTRY next = link->Flink;
            PF_REQUEST_CONTEXT* ctx = PfRequestFromLink(link);
            if (ctx->Device == Device && (Endpoint == nullptr || ctx->Endpoint == Endpoint)) {
                RemoveEntryList(link);
                if (file != nullptr) {
                    PF_STANDALONE_RECORD* rec = PfAllocStandaloneRecord(Ctrl, PFVHCI_REC_CANCEL, ctx->DeviceHandle,
                                                                        ctx->Cookie, ctx->RequestId);
                    if (rec != nullptr) {
                        rec->Op.Endpoint = ctx->EpAddress;
                        if (!PfInsertStandaloneLocked(Ctrl, file, rec)) {
                            PfFreeStandaloneRecord(rec);
                            ++cancelRecordsLost;
                        }
                    } else {
                        ++cancelRecordsLost;
                    }
                }
                if (PfTakeOwnershipLocked(ctx, STATUS_CANCELLED, USBD_STATUS_CANCELED)) {
                    InsertTailList(&owned, &ctx->Entry.Link);
                }
            }
            link = next;
        }
    }

    if (file != nullptr && !IsListEmpty(&file->Records)) {
        pumpFile = dev->Owner;
        WdfObjectReference(pumpFile);
    }

    WdfSpinLockRelease(Ctrl->Lock);

    if (cancelRecordsLost != 0) {
        PF_WARN("abort: %u CANCEL records could not be queued\n", cancelRecordsLost);
    }

    /* Purge/abort run inside UdeCx callbacks: complete through the DPC. */
    PfCompleteListAsync(Ctrl, &owned);

    if (pumpFile != nullptr) {
        PfPumpFile(Ctrl, pumpFile);
        WdfObjectDereference(pumpFile);
    }
}

_Requires_lock_held_(Ctrl->Lock)
VOID PfCollectDeviceRequestsLocked(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_opt_ PF_FILE_CONTEXT* Owner,
                                   _In_ UDECXUSBDEVICE Device, _Inout_ PLIST_ENTRY OwnedList)
{
    if (Owner != nullptr) {
        PLIST_ENTRY link = Owner->Records.Flink;
        while (link != &Owner->Records) {
            PLIST_ENTRY next = link->Flink;
            PF_QUEUE_ENTRY* entry = PfEntryFromLink(link);
            if (entry->Kind == PfEntryKind::Request && PfRequestFromEntry(entry)->Device == Device) {
                PF_REQUEST_CONTEXT* ctx = PfRequestFromEntry(entry);
                NTSTATUS nt;
                USBD_STATUS usbd;
                PfGoneStatus(ctx, &nt, &usbd);
                RemoveEntryList(link);
                if (PfTakeOwnershipLocked(ctx, nt, usbd)) {
                    InsertTailList(OwnedList, &ctx->Entry.Link);
                }
            }
            link = next;
        }
    }

    for (ULONG b = 0; b < PF_INFLIGHT_BUCKETS; ++b) {
        PLIST_ENTRY head = &Ctrl->InFlight[b];
        PLIST_ENTRY link = head->Flink;
        while (link != head) {
            PLIST_ENTRY next = link->Flink;
            PF_REQUEST_CONTEXT* ctx = PfRequestFromLink(link);
            if (ctx->Device == Device) {
                NTSTATUS nt;
                USBD_STATUS usbd;
                PfGoneStatus(ctx, &nt, &usbd);
                RemoveEntryList(link);
                if (PfTakeOwnershipLocked(ctx, nt, usbd)) {
                    InsertTailList(OwnedList, &ctx->Entry.Link);
                }
            }
            link = next;
        }
    }
    /* Requests in State Fetching are failed by PfFinalizeFetchedLocked (device is Gone). */
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID PfPumpFile(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ WDFFILEOBJECT FileObject)
{
    PF_FILE_CONTEXT* file = PfGetFileContext(FileObject);

    WdfSpinLockAcquire(Ctrl->Lock);
    if (file->PumpActive) {
        /*
         * Another thread is pumping this file; it re-checks the record list and the
         * FETCH queue under the lock before it stops, so our new work is not lost.
         * One active pump per file also keeps FETCH completions in record order.
         */
        WdfSpinLockRelease(Ctrl->Lock);
        return;
    }
    file->PumpActive = TRUE;

    for (;;) {
        if (file->Closing || IsListEmpty(&file->Records)) {
            break;
        }
        WDFREQUEST fetch;
        const NTSTATUS st = WdfIoQueueRetrieveRequestByFileObject(Ctrl->FetchQueue, FileObject, &fetch);
        if (!NT_SUCCESS(st)) {
            break; /* STATUS_NO_MORE_ENTRIES: records wait for the next FETCH */
        }
        WdfSpinLockRelease(Ctrl->Lock);

        PfFillFetch(Ctrl, file, fetch);

        WdfSpinLockAcquire(Ctrl->Lock);
    }

    file->PumpActive = FALSE;
    WdfSpinLockRelease(Ctrl->Lock);
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfFetchArrived(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File, _In_ WDFREQUEST Request)
{
    PAGED_CODE();

    const NTSTATUS st = WdfRequestForwardToIoQueue(Request, Ctrl->FetchQueue);
    if (!NT_SUCCESS(st)) {
        PF_ERROR("FETCH: WdfRequestForwardToIoQueue 0x%08X\n", st);
        WdfRequestCompleteWithInformation(Request, st, 0);
        return;
    }

    /* Closing is only ever set from FALSE to TRUE; a stale read just delays the check below. */
    if (File->Closing) {
        /* Raced with EvtFileCleanup, which may already have drained the queue. */
        PfCancelFetches(Ctrl, File->FileObject);
        return;
    }

    PfPumpFile(Ctrl, File->FileObject);
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED NTSTATUS PfProcessCompleteBuffer(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _In_ PF_FILE_CONTEXT* File,
                                          _In_reads_bytes_(Length) const UCHAR* Buffer, _In_ size_t Length,
                                          _Out_ size_t* Consumed)
{
    PAGED_CODE();

    *Consumed = 0;
    size_t offset = 0;

    while (offset < Length) {
        const size_t remaining = Length - offset;
        if (remaining < sizeof(PFVHCI_RECORD_HEADER)) {
            PF_WARN("COMPLETE: %Iu trailing bytes at offset %Iu\n", remaining, offset);
            return STATUS_INVALID_PARAMETER;
        }

        PFVHCI_RECORD_HEADER header;
        RtlCopyMemory(&header, Buffer + offset, sizeof(header)); /* single fetch from shared memory */

        if (header.Size < sizeof(PFVHCI_RECORD_HEADER) || (header.Size & 7u) != 0 || header.Size > remaining ||
            header.Flags != 0 || header.Reserved != 0) {
            PF_WARN("COMPLETE: bad record header at offset %Iu (size %u type %u)\n", offset, header.Size,
                    header.Type);
            return STATUS_INVALID_PARAMETER;
        }

        switch (header.Type) {
        case PFVHCI_REC_COMPLETION:
            if (header.Size < sizeof(PFVHCI_COMPLETION)) {
                return STATUS_INVALID_PARAMETER;
            }
            PfApplyCompletion(Ctrl, File, Buffer + offset, header.Size);
            break;

        case PFVHCI_REC_OP_RESULT:
            if (header.Size != sizeof(PFVHCI_OP_RESULT)) {
                return STATUS_INVALID_PARAMETER;
            }
            PfApplyOpResult(Ctrl, File, Buffer + offset);
            break;

        default:
            PF_WARN("COMPLETE: unexpected record type 0x%04X at offset %Iu\n", header.Type, offset);
            return STATUS_INVALID_PARAMETER;
        }

        offset += header.Size;
        *Consumed = offset;
    }
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
PF_PAGED VOID PfFileRecordsCleanup(_In_ PF_CONTROLLER_CONTEXT* Ctrl, _Inout_ PF_FILE_CONTEXT* File)
{
    PAGED_CODE();

    LIST_ENTRY records;
    InitializeListHead(&records);
    LIST_ENTRY owned;
    InitializeListHead(&owned);

    /* The spinlock section lives in a nonpaged helper (a paged function must not raise IRQL). */
    PfDetachAllFileRecords(Ctrl, File, &records, &owned);

    while (!IsListEmpty(&records)) {
        PfFreeStandaloneRecord(PfStandaloneFromEntry(PfEntryFromLink(RemoveHeadList(&records))));
    }
    PfCompleteListNow(&owned);

    PfCancelFetches(Ctrl, File->FileObject);
}
