/*
 * ParsFlash USB Redirector - pfvhci user/kernel ABI
 * https://parsflash.com
 *
 * Shared by:
 *   - Driver/Windows/KMDF (pfvhci.sys, kernel mode, C++ without exceptions)
 *   - Client/VirtualUSB   (user mode, C++20)
 *   - Tests               (Linux/Wine test doubles)
 *
 * All structures are little-endian, naturally aligned, and every record in a
 * FETCH/COMPLETE buffer starts at an 8-byte aligned offset. Bump PFVHCI_ABI_VERSION
 * on any incompatible change; the client refuses to run against a different ABI.
 *
 * See Docs/DRIVER.md for semantics.
 */
#ifndef PFVHCI_IOCTL_H
#define PFVHCI_IOCTL_H

#if defined(_WIN32) || defined(_KERNEL_MODE)
#  if !defined(_KERNEL_MODE)
#    include <windows.h>
#    include <winioctl.h>
#  endif
#else
/* Non-Windows hosts (unit tests only). */
#  include <stdint.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int32_t INT32;
#  define FILE_DEVICE_UNKNOWN 0x00000022
#  define METHOD_BUFFERED 0
#  define METHOD_IN_DIRECT 1
#  define METHOD_OUT_DIRECT 2
#  define FILE_READ_ACCESS 0x0001
#  define FILE_WRITE_ACCESS 0x0002
#  define CTL_CODE(DeviceType, Function, Method, Access) \
      (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PFVHCI_ABI_VERSION 1u

/* {5D1D9FCF-12B3-4DDF-BB2B-3B2E8EBB2584} - private control interface of pfvhci.
 * Declared with DEFINE_GUID where available. */
#define PFVHCI_INTERFACE_GUID_INIT \
    { 0x5d1d9fcf, 0x12b3, 0x4ddf, { 0xbb, 0x2b, 0x3b, 0x2e, 0x8e, 0xbb, 0x25, 0x84 } }

#define PFVHCI_HARDWARE_ID L"Root\\ParsFlashVhci"

/* Function codes 0x900.. are in the vendor range. */
#define PFVHCI_IOCTL(fn, method, access) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900 + (fn), (method), (access))

/* in: none                       out: PFVHCI_VERSION_INFO */
#define IOCTL_PFVHCI_GET_VERSION PFVHCI_IOCTL(0, METHOD_BUFFERED, FILE_READ_ACCESS)
/* in: PFVHCI_PLUGIN_HEADER + entries  out: PFVHCI_PLUGIN_RESULT */
#define IOCTL_PFVHCI_PLUGIN      PFVHCI_IOCTL(1, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
/* in: PFVHCI_UNPLUG             out: none */
#define IOCTL_PFVHCI_UNPLUG      PFVHCI_IOCTL(2, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
/* in: none                       out: buffer of records (pended until >= 1 record available) */
#define IOCTL_PFVHCI_FETCH       PFVHCI_IOCTL(3, METHOD_OUT_DIRECT, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
/* in: buffer of COMPLETION/OP_RESULT records   out: none */
#define IOCTL_PFVHCI_COMPLETE    PFVHCI_IOCTL(4, METHOD_IN_DIRECT, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
/* in: none                       out: PFVHCI_PORT_LIST (variable) */
#define IOCTL_PFVHCI_GET_PORTS   PFVHCI_IOCTL(5, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Speeds use the same values as pf::UsbSpeed. */
#define PFVHCI_SPEED_LOW        1
#define PFVHCI_SPEED_FULL       2
#define PFVHCI_SPEED_HIGH       3
#define PFVHCI_SPEED_SUPER      4
#define PFVHCI_SPEED_SUPER_PLUS 5

/* Transfer types (= USB bmAttributes & 3 = pf::TransferType). */
#define PFVHCI_XFER_CONTROL     0
#define PFVHCI_XFER_ISOCHRONOUS 1
#define PFVHCI_XFER_BULK        2
#define PFVHCI_XFER_INTERRUPT   3

/* Transfer flags (= pf::xfer_flags). */
#define PFVHCI_XF_SHORT_NOT_OK 0x0001
#define PFVHCI_XF_ZERO_PACKET  0x0002
#define PFVHCI_XF_DIR_IN       0x0004

/* USB status codes (= pf::UsbStatus). */
#define PFVHCI_STATUS_SUCCESS       0
#define PFVHCI_STATUS_STALL         1
#define PFVHCI_STATUS_TIMEOUT       2
#define PFVHCI_STATUS_CANCELLED     3
#define PFVHCI_STATUS_OVERFLOW      4
#define PFVHCI_STATUS_SHORT_PACKET  5
#define PFVHCI_STATUS_DEVICE_GONE   6
#define PFVHCI_STATUS_NO_DEVICE     7
#define PFVHCI_STATUS_INVALID       8
#define PFVHCI_STATUS_NOT_SUPPORTED 9
#define PFVHCI_STATUS_IO_ERROR      10

#define PFVHCI_ALIGN8(x) (((x) + 7u) & ~7u)

#pragma pack(push, 8)

typedef struct _PFVHCI_VERSION_INFO {
    UINT32 AbiVersion;
    UINT32 DriverMajor;
    UINT32 DriverMinor;
    UINT32 DriverPatch;
    UINT32 DriverBuild;
    UINT32 NumUsb2Ports;
    UINT32 NumUsb3Ports;
    UINT32 Reserved;
} PFVHCI_VERSION_INFO;

/* Descriptor entry types inside a PLUGIN request. Type uses the USB descriptor
 * type code: 0x01 device, 0x02 configuration, 0x03 string, 0x06 qualifier, 0x0F BOS. */
typedef struct _PFVHCI_DESCRIPTOR_ENTRY {
    UINT8 Type;
    UINT8 Index;        /* configuration index or string index */
    UINT16 LangId;      /* strings only */
    UINT32 Length;      /* bytes of descriptor data following this header */
    /* UINT8 Data[Length]; padded to 8 bytes */
} PFVHCI_DESCRIPTOR_ENTRY;

typedef struct _PFVHCI_PLUGIN_HEADER {
    UINT32 Size;             /* total input size incl. entries */
    UINT32 AbiVersion;       /* PFVHCI_ABI_VERSION */
    UINT64 Cookie;           /* opaque user-mode value echoed in every record */
    UINT8 Speed;             /* PFVHCI_SPEED_* */
    UINT8 Reserved0[3];
    UINT32 EntryCount;
    /* PFVHCI_DESCRIPTOR_ENTRY entries[EntryCount], each 8-aligned */
} PFVHCI_PLUGIN_HEADER;

typedef struct _PFVHCI_PLUGIN_RESULT {
    UINT32 DeviceHandle;     /* driver-assigned, non-zero */
    UINT32 PortNumber;       /* 1-based virtual root port */
} PFVHCI_PLUGIN_RESULT;

typedef struct _PFVHCI_UNPLUG {
    UINT32 DeviceHandle;     /* 0 = all devices owned by this handle */
    UINT32 Reserved;
} PFVHCI_UNPLUG;

typedef struct _PFVHCI_PORT_ENTRY {
    UINT32 PortNumber;
    UINT32 DeviceHandle;     /* 0 = empty */
    UINT64 Cookie;
    UINT8 Speed;
    UINT8 Reserved[7];
} PFVHCI_PORT_ENTRY;

typedef struct _PFVHCI_PORT_LIST {
    UINT32 Count;
    UINT32 Reserved;
    PFVHCI_PORT_ENTRY Ports[1];
} PFVHCI_PORT_LIST;

/* ---- Record stream (FETCH: kernel -> user, COMPLETE: user -> kernel) ---- */

#define PFVHCI_REC_SUBMIT           0x0001  /* URB to execute on the remote device */
#define PFVHCI_REC_CANCEL           0x0002  /* a previously fetched SUBMIT was cancelled by the OS */
#define PFVHCI_REC_ENDPOINT_RESET   0x0003  /* clear halt / pipe reset; complete with OP_RESULT */
#define PFVHCI_REC_SELECT_CONFIG    0x0004  /* SET_CONFIGURATION; complete with OP_RESULT */
#define PFVHCI_REC_SELECT_INTERFACE 0x0005  /* SET_INTERFACE; complete with OP_RESULT */
#define PFVHCI_REC_DEVICE_RESET     0x0006  /* port reset requested by the hub driver */
#define PFVHCI_REC_DEVICE_REMOVED   0x0007  /* informational: device unplugged (e.g. by PnP) */
#define PFVHCI_REC_COMPLETION       0x0081  /* user -> kernel: SUBMIT finished */
#define PFVHCI_REC_OP_RESULT        0x0082  /* user -> kernel: non-transfer op finished */

typedef struct _PFVHCI_RECORD_HEADER {
    UINT32 Size;           /* total record size including header, multiple of 8 */
    UINT16 Type;           /* PFVHCI_REC_* */
    UINT16 Flags;          /* reserved, 0 */
    UINT32 DeviceHandle;
    UINT32 Reserved;
    UINT64 Cookie;         /* from PLUGIN */
    UINT64 RequestId;      /* driver-unique id; echoed by COMPLETION/OP_RESULT */
} PFVHCI_RECORD_HEADER;   /* 32 bytes */

typedef struct _PFVHCI_ISO_PACKET {
    UINT32 Offset;
    UINT32 Length;
} PFVHCI_ISO_PACKET;

typedef struct _PFVHCI_SUBMIT {
    PFVHCI_RECORD_HEADER Header;
    UINT8 Endpoint;        /* bEndpointAddress (0x00 or 0x80 for EP0) */
    UINT8 TransferType;    /* PFVHCI_XFER_* */
    UINT16 TransferFlags;  /* PFVHCI_XF_* */
    UINT32 TransferLength; /* IN: requested length, OUT: bytes of data that follow */
    UINT32 TimeoutMs;      /* 0 = none */
    UINT32 IsoPacketCount;
    UINT8 Setup[8];        /* control transfers only */
    /* PFVHCI_ISO_PACKET Iso[IsoPacketCount]; then OUT data[TransferLength]; padded to 8 */
} PFVHCI_SUBMIT;          /* 56 bytes */

typedef struct _PFVHCI_ENDPOINT_OP {
    PFVHCI_RECORD_HEADER Header;
    UINT8 Endpoint;          /* ENDPOINT_RESET */
    UINT8 ConfigurationValue;/* SELECT_CONFIG (0 = unconfigure) */
    UINT8 InterfaceNumber;   /* SELECT_INTERFACE */
    UINT8 AlternateSetting;  /* SELECT_INTERFACE */
    UINT32 Reserved;
} PFVHCI_ENDPOINT_OP;     /* 40 bytes; used by REC_ENDPOINT_RESET, REC_SELECT_CONFIG, REC_SELECT_INTERFACE, REC_DEVICE_RESET, REC_DEVICE_REMOVED, REC_CANCEL */

typedef struct _PFVHCI_ISO_RESULT {
    UINT32 ActualLength;
    INT32 Status;
} PFVHCI_ISO_RESULT;

typedef struct _PFVHCI_COMPLETION {
    PFVHCI_RECORD_HEADER Header;
    INT32 Status;          /* PFVHCI_STATUS_* */
    UINT32 ActualLength;   /* bytes transferred (IN: bytes of data that follow) */
    UINT32 IsoPacketCount;
    UINT32 IsoErrorCount;
    /* PFVHCI_ISO_RESULT Iso[IsoPacketCount]; then IN data[ActualLength]; padded to 8 */
} PFVHCI_COMPLETION;      /* 48 bytes */

typedef struct _PFVHCI_OP_RESULT {
    PFVHCI_RECORD_HEADER Header;
    INT32 Status;
    UINT32 Reserved;
} PFVHCI_OP_RESULT;       /* 40 bytes */

#pragma pack(pop)

/* Upper bounds enforced by the driver. */
#define PFVHCI_MAX_DESCRIPTOR_BYTES (64u * 1024u)
#define PFVHCI_MAX_TRANSFER_BYTES   (16u * 1024u * 1024u)
#define PFVHCI_MAX_ISO_PACKETS      1024u
#define PFVHCI_MIN_FETCH_BUFFER     (64u * 1024u)

#ifdef __cplusplus
} /* extern "C" */

static_assert(sizeof(PFVHCI_RECORD_HEADER) == 32, "ABI");
static_assert(sizeof(PFVHCI_SUBMIT) == 56, "ABI");
static_assert(sizeof(PFVHCI_ENDPOINT_OP) == 40, "ABI");
static_assert(sizeof(PFVHCI_COMPLETION) == 48, "ABI");
static_assert(sizeof(PFVHCI_OP_RESULT) == 40, "ABI");
static_assert(sizeof(PFVHCI_PLUGIN_HEADER) == 24, "ABI");
static_assert(sizeof(PFVHCI_DESCRIPTOR_ENTRY) == 8, "ABI");
#endif

#endif /* PFVHCI_IOCTL_H */
