/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Common.h - system includes, constants and every object context of the driver.
 *
 * Object model (UdeCx):
 *
 *   WDFDEVICE (controller, root-enumerated)       PF_CONTROLLER_CONTEXT
 *     +-- WDFFILEOBJECT (one per open handle)     PF_FILE_CONTEXT
 *     +-- UDECXUSBDEVICE (one per plugged device) PF_DEVICE_CONTEXT    (owned by a file object)
 *           +-- UDECXUSBENDPOINT                  PF_ENDPOINT_CONTEXT
 *                 +-- WDFQUEUE (URB queue)        PF_QUEUE_CONTEXT
 *   WDFREQUEST (URB / UdeCx op requests)          PF_REQUEST_CONTEXT   (allocated lazily)
 *
 * Locking: one WDFSPINLOCK per controller (PF_CONTROLLER_CONTEXT::Lock) protects
 *   - the port table, device states and owner links,
 *   - every per-file record list and PumpActive flag,
 *   - the in-flight hash table and the DPC completion list,
 *   - PF_REQUEST_CONTEXT::State / CancelRoutineRan,
 *   - device endpoint lists and PF_ENDPOINT_CONTEXT::AbortSequence.
 * Hold times are short (list manipulation only). Data copies and request
 * completion always happen with the lock released.
 *
 * PF_CONTROLLER_CONTEXT::PlugLock (WDFWAITLOCK, PASSIVE_LEVEL) serializes the
 * plug-in / unplug state machine (PLUGIN, UNPLUG, EvtFileCleanup, QueryRemove).
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include <ntstrsafe.h>

#include <usb.h>
#include <usbioctl.h>

/*
 * initguid.h makes DEFINE_GUID emit DECLSPEC_SELECTANY definitions, so the GUIDs
 * from usbiodef.h (GUID_DEVINTERFACE_USB_HOST_CONTROLLER) and usbdlib.h
 * (GUID_USB_CAPABILITY_*) are defined in every translation unit without
 * duplicate-symbol errors.
 */
#include <initguid.h>
#include <usbiodef.h>
extern "C" {
#include <usbdlib.h>
}
/* VERIFY: if GUID_USB_CAPABILITY_* are unresolved with the installed WDK, also include <usbbusif.h>. */

#include <wdfusb.h> /* WDF_USB_CONTROL_SETUP_PACKET, required by udecxurb.h */
#include <Udecx.h>

#include "pfvhci_ioctl.h"
#include "Trace.h"
#include "Version.h"

/* ------------------------------------------------------------------------ */
/* Code sections                                                            */
/* ------------------------------------------------------------------------ */

/*
 * #pragma alloc_text cannot name C++ (mangled) functions, so pageable functions
 * are placed with __declspec(code_seg). Only functions that are guaranteed to run
 * at PASSIVE_LEVEL are paged; every callback that UdeCx or WDF may invoke at
 * DISPATCH_LEVEL stays in nonpaged .text.
 */
#ifdef ALLOC_PRAGMA
#define PF_PAGED __declspec(code_seg("PAGE"))
#define PF_INIT  __declspec(code_seg("INIT"))
#else
#define PF_PAGED
#define PF_INIT
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

constexpr ULONG PF_POOL_TAG = 'hvfP'; /* "Pfvh" in !pool output */

/* UdeCx root hub limits: per type 1..PF_MAX_PORTS_PER_TYPE, total <= PF_MAX_PORTS. */
constexpr ULONG PF_DEFAULT_USB2_PORTS = 8;
constexpr ULONG PF_DEFAULT_USB3_PORTS = 8;
constexpr ULONG PF_MAX_PORTS_PER_TYPE = 127;
constexpr ULONG PF_MAX_PORTS = 2 * PF_MAX_PORTS_PER_TYPE;

/* In-flight table: RequestId -> request. Power of two. */
constexpr ULONG PF_INFLIGHT_BUCKETS = 64;

/* PLUGIN input bounds (driver policy, on top of PFVHCI_MAX_DESCRIPTOR_BYTES per descriptor). */
constexpr ULONG PF_MAX_PLUGIN_BYTES = 1024u * 1024u;
constexpr ULONG PF_MAX_DESCRIPTOR_ENTRIES = 1024;

/*
 * Upper bound of informational (CANCEL / DEVICE_REMOVED) records waiting for a
 * FETCH per file object; protects nonpaged pool if user mode stops fetching.
 * Request-backed records need no bound: each is an outstanding OS request.
 */
constexpr ULONG PF_MAX_STANDALONE_RECORDS_PER_FILE = 4096;

/* Reference string of the private interface; opens through it are "control" handles. */
#define PF_PRIVATE_REFERENCE_STRING L"PFVHCI"

/* USB standard request codes and bmRequestType pieces (USB 2.0 spec, chapter 9). */
constexpr UCHAR PF_REQ_GET_STATUS = 0x00;
constexpr UCHAR PF_REQ_CLEAR_FEATURE = 0x01;
constexpr UCHAR PF_REQ_SET_FEATURE = 0x03;
constexpr UCHAR PF_REQ_GET_DESCRIPTOR = 0x06;
constexpr UCHAR PF_REQ_SET_DESCRIPTOR = 0x07;
constexpr UCHAR PF_REQ_GET_CONFIGURATION = 0x08;
constexpr UCHAR PF_REQ_GET_INTERFACE = 0x0A;
constexpr UCHAR PF_RT_DIR_IN = 0x80;
constexpr UCHAR PF_RT_TYPE_STANDARD = 0x00;
constexpr UCHAR PF_RT_TYPE_CLASS = 0x20;
constexpr UCHAR PF_RT_TYPE_VENDOR = 0x40;
constexpr UCHAR PF_RT_RECIP_DEVICE = 0x00;
constexpr UCHAR PF_RT_RECIP_INTERFACE = 0x01;
constexpr UCHAR PF_RT_RECIP_ENDPOINT = 0x02;
constexpr UCHAR PF_RT_RECIP_OTHER = 0x03;

/* USB descriptor type codes used by PLUGIN entries. */
constexpr UCHAR PF_DESC_DEVICE = 0x01;
constexpr UCHAR PF_DESC_CONFIGURATION = 0x02;
constexpr UCHAR PF_DESC_STRING = 0x03;
constexpr UCHAR PF_DESC_ENDPOINT = 0x05;
constexpr UCHAR PF_DESC_DEVICE_QUALIFIER = 0x06;
constexpr UCHAR PF_DESC_BOS = 0x0F;

/* ------------------------------------------------------------------------ */
/* State enums                                                              */
/* ------------------------------------------------------------------------ */

enum class PfDeviceState : UCHAR {
    Creating = 0, /* UDECXUSBDEVICE created, UdecxUsbDevicePlugIn not yet called */
    PluggedIn,    /* accepting URBs */
    Gone          /* unplug started; every new URB fails with DEVICE_GONE */
};

/*
 * Life cycle of a request-backed record (URB or UdeCx operation request).
 *
 *   Idle --submit--> Queued --FETCH copies it--> Fetching --> InFlight --COMPLETE--> Owned
 *     |                 |                           |            |
 *     |                 +--cancel/abort--> Owned    |            +--cancel/abort--> Owned
 *     +--immediate failure--> Owned                 +--cancel/abort/unplug noticed when the
 *                                                      FETCH path finalizes (lock re-acquired)
 *
 * "Owned" means: removed from every list; exactly one party (the thread that took it
 * out, or the EvtRequestCancel routine) completes it. See RecordQueue.cpp.
 */
enum class PfReqState : UCHAR {
    Idle = 0,
    Queued,   /* in PF_FILE_CONTEXT::Records, not yet delivered to user mode */
    Fetching, /* being copied into a FETCH buffer (lock released) */
    InFlight, /* delivered; in PF_CONTROLLER_CONTEXT::InFlight */
    Owned     /* out of all lists, being completed */
};

enum class PfRecKind : UCHAR {
    Submit = 0,      /* PFVHCI_REC_SUBMIT        <- URB */
    EndpointReset,   /* PFVHCI_REC_ENDPOINT_RESET <- EvtUsbEndpointReset or SYNC_RESET_PIPE URB */
    SelectConfig,    /* PFVHCI_REC_SELECT_CONFIG  <- EvtUsbDeviceEndpointsConfigure */
    SelectInterface, /* PFVHCI_REC_SELECT_INTERFACE */
    DeviceReset      /* PFVHCI_REC_DEVICE_RESET   <- EvtUsbDeviceReset */
};

enum class PfEntryKind : UCHAR { Request = 0, Standalone };

enum class PfUnplugReason : UCHAR {
    UserRequest = 0,    /* IOCTL_PFVHCI_UNPLUG */
    OwnerClosed,        /* EvtFileCleanup of the owning handle */
    ControllerRemoval   /* PnP is removing the virtual controller */
};

/* ------------------------------------------------------------------------ */
/* Record queue entries                                                     */
/* ------------------------------------------------------------------------ */

struct PF_QUEUE_ENTRY {
    LIST_ENTRY Link;
    PfEntryKind Kind;
};

/* Informational records (CANCEL, DEVICE_REMOVED) that have no WDFREQUEST behind them. */
struct PF_STANDALONE_RECORD {
    PF_QUEUE_ENTRY Entry;
    WDFMEMORY Memory;       /* lookaside allocation that holds this structure */
    PFVHCI_ENDPOINT_OP Op;  /* the complete 40-byte record as delivered to user mode */
};

/* ------------------------------------------------------------------------ */
/* Contexts                                                                 */
/* ------------------------------------------------------------------------ */

struct PF_REQUEST_CONTEXT {
    PF_QUEUE_ENTRY Entry;        /* Records list / in-flight bucket / completion list / local lists */
    WDFREQUEST Request;
    UDECXUSBDEVICE Device;
    UDECXUSBENDPOINT Endpoint;   /* NULL for device-level operations */
    PURB Urb;                    /* NULL if not IOCTL_INTERNAL_USB_SUBMIT_URB */

    UINT64 RequestId;
    UINT64 Cookie;               /* copied from the device at submit time */
    ULONG DeviceHandle;
    ULONG AbortSequence;         /* PF_ENDPOINT_CONTEXT::AbortSequence when the URB was dispatched */

    PfRecKind RecKind;
    PfReqState State;
    BOOLEAN CancelRoutineRan;    /* EvtRequestCancel ran while State == Fetching */
    BOOLEAN IsUrb;
    BOOLEAN SetBytes;            /* URB type has TransferBufferLength -> UdecxUrbSetBytesCompleted */
    BOOLEAN DirIn;

    /* SUBMIT description */
    UINT8 EpAddress;             /* bEndpointAddress incl. direction bit */
    UINT8 XferType;              /* PFVHCI_XFER_* */
    UINT16 XferFlags;            /* PFVHCI_XF_* */
    ULONG TransferLength;        /* bytes requested (IN) / bytes of OUT data */
    ULONG TimeoutMs;
    ULONG IsoPacketCount;
    UCHAR Setup[8];
    PUCHAR Buffer;               /* system VA of the URB transfer buffer (may be NULL if length 0) */

    /* Operation parameters (ENDPOINT_RESET / SELECT_CONFIG / SELECT_INTERFACE) */
    UINT8 OpEndpoint;
    UINT8 OpConfigurationValue;
    UINT8 OpInterfaceNumber;
    UINT8 OpAlternateSetting;

    /* Completion values (valid once State == Owned) */
    NTSTATUS CompleteNt;
    USBD_STATUS CompleteUsbd;
    ULONG BytesCompleted;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PF_REQUEST_CONTEXT, PfGetRequestContext)

struct PF_CONTROLLER_CONTEXT {
    WDFDEVICE Device;
    WDFSPINLOCK Lock;
    WDFWAITLOCK PlugLock;
    WDFQUEUE DefaultQueue;
    WDFQUEUE FetchQueue;         /* manual queue of pended IOCTL_PFVHCI_FETCH */
    WDFDPC CompletionDpc;
    WDFLOOKASIDE RecordLookaside;/* PF_STANDALONE_RECORD allocations */

    LIST_ENTRY CompletionList;   /* PF_REQUEST_CONTEXT waiting for the DPC */
    LIST_ENTRY InFlight[PF_INFLIGHT_BUCKETS];

    ULONG NumUsb2Ports;          /* root hub ports 1..NumUsb2Ports */
    ULONG NumUsb3Ports;          /* root hub ports NumUsb2Ports+1..NumUsb2Ports+NumUsb3Ports */
    UDECXUSBDEVICE Ports[PF_MAX_PORTS];  /* index = port - 1; holds a WDF reference */
    BOOLEAN PortReserved[PF_MAX_PORTS];  /* reserved by an in-progress PLUGIN */

    ULONG NextDeviceHandle;
    UINT64 NextRequestId;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PF_CONTROLLER_CONTEXT, PfGetControllerContext)

struct PF_FILE_CONTEXT {
    WDFFILEOBJECT FileObject;
    WDFDEVICE Controller;
    LIST_ENTRY Records;          /* FIFO of PF_QUEUE_ENTRY waiting for a FETCH */
    LIST_ENTRY Devices;          /* PF_DEVICE_CONTEXT::OwnerLink */
    ULONG StandaloneCount;
    BOOLEAN IsControl;           /* opened through the private interface (may use IOCTL_PFVHCI_*) */
    BOOLEAN Closing;             /* EvtFileCleanup has started */
    BOOLEAN PumpActive;          /* a thread is currently filling FETCH requests for this file */
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PF_FILE_CONTEXT, PfGetFileContext)

struct PF_DEVICE_CONTEXT {
    UDECXUSBDEVICE Handle;
    WDFDEVICE Controller;
    WDFFILEOBJECT Owner;         /* NULL once the device is Gone */
    LIST_ENTRY OwnerLink;
    LIST_ENTRY Endpoints;        /* PF_ENDPOINT_CONTEXT::DeviceLink */
    WDFMEMORY Descriptors;       /* private copy of the PLUGIN input; lives as long as the device */
    UINT64 Cookie;
    ULONG DeviceHandle;
    ULONG Port;                  /* 1-based root hub port, 0 if none */
    UINT8 Speed;                 /* PFVHCI_SPEED_* */
    PfDeviceState State;
    BOOLEAN OwnerLinked;
    BOOLEAN HasMsOsVendorCode;
    UINT8 MsOsVendorCode;        /* bMS_VendorCode from string descriptor 0xEE */
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PF_DEVICE_CONTEXT, PfGetDeviceContext)

struct PF_ENDPOINT_CONTEXT {
    UDECXUSBENDPOINT Handle;
    UDECXUSBDEVICE Device;
    WDFQUEUE Queue;
    LIST_ENTRY DeviceLink;
    USBD_PIPE_HANDLE PipeHandle; /* learned from transfer URBs; maps pipe requests to endpoints */
    ULONG AbortSequence;         /* incremented (lock held) by purge / ABORT_PIPE */
    UINT8 Address;              /* bEndpointAddress (0 for the default control endpoint) */
    UINT8 TransferType;          /* PFVHCI_XFER_* */
    BOOLEAN Linked;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PF_ENDPOINT_CONTEXT, PfGetEndpointContext)

struct PF_QUEUE_CONTEXT {
    UDECXUSBENDPOINT Endpoint;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PF_QUEUE_CONTEXT, PfGetQueueContext)

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

inline ULONG64 PfAlign8(ULONG64 Value)
{
    return (Value + 7ull) & ~7ull;
}

inline BOOLEAN PfIsSuperSpeed(UINT8 Speed)
{
    return Speed == PFVHCI_SPEED_SUPER || Speed == PFVHCI_SPEED_SUPER_PLUS;
}
