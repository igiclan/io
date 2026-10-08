# pfvhci.sys — ParsFlash Virtual USB Host Controller

Client-side kernel driver of the ParsFlash USB Redirector (https://parsflash.com).
It presents a virtual USB host controller to Windows. Remote USB devices exported by a
ParsFlash Server are plugged into it, so that unmodified Windows software and drivers
(WinUSB, usbser, ADB, fastboot, QFIL, SP Flash Tool, Odin, …) use them like local devices.

Sources: [`Driver/Windows/KMDF/pfvhci/`](../Driver/Windows/KMDF/pfvhci),
installer helper [`Driver/Windows/pfdevcon/`](../Driver/Windows/pfdevcon),
user/kernel ABI [`Shared/Driver/pfvhci_ioctl.h`](../Shared/Driver/pfvhci_ioctl.h) (frozen, `PFVHCI_ABI_VERSION 1`).

---

## 0. STATUS — read this first

| Component | Status (legend in [ARCHITECTURE.md](ARCHITECTURE.md)) |
|---|---|
| `pfvhci.sys` sources, INF, vcxproj | **UNBUILT** |
| `pfdevcon.exe` | **BUILT** (MinGW-w64 cross build, zero warnings; runs under Wine; never run against the real driver) |

- **The driver source was written without access to the WDK, MSVC or a Windows machine.
  It has NOT been compiled, linked, loaded or tested in any environment.** The only check
  made was a C++17 syntax/type-consistency pass (`g++ -fsyntax-only -Wall -Wextra -Wconversion`)
  against hand-written stub headers that model the WDK declarations *as the author
  understood them*. That proves nothing about the real WDK signatures.
- API usage was cross-checked against the Microsoft DDI reference sources
  (MicrosoftDocs/windows-driver-docs-ddi), the "Write a UDE client driver" guide and the
  open-source UdeCx driver usbip-win2 (no code copied). Points that could not be verified
  are marked `// VERIFY:` in the source and listed in [§17](#17-open-verify-items).
- Before any use, the driver **must** be built with VS 2022 + WDK, analysed with Code
  Analysis / SDV, and exercised on a dedicated test machine **under Driver Verifier**
  (see [§15](#15-debugging)). Nothing in this document is a claim that the driver works.
- Expect the first build to need fixes (signature mismatches, warnings as errors).

---

## 1. Why UdeCx

Windows loads USB function drivers only for devices that a *host controller* reports on a
*hub*. Two ways exist to emulate one:

| Approach | Verdict |
|---|---|
| Raw WDM bus driver that fakes usbhub/usbport contracts (classic usbip-win "vhci") | Undocumented contracts, breaks between Windows builds. Rejected. |
| **KMDF + USB Device Emulation class extension (UdeCx, `udecx.sys`, inbox since Windows 10 1607)** | Documented and supported; Windows supplies the real `usbhub3`/UCX stack above the emulated controller; the driver implements only controller/device/endpoint callbacks. Same approach as usbip-win2. **Chosen.** |

## 2. Supported Windows versions

- Windows 10 **x64, version 1709 (build 16299) or later**, and Windows 11 x64.
- Build settings: `_NT_TARGET_VERSION = 0xA000004` (RS3), **KMDF 1.23**, **UdeCx 1.1**
  (needed for `NumberOfUsb20Ports` / `NumberOfUsb30Ports`).
- INF models section `NTamd64.10.0...16299`; no x86 or ARM64 package.

## 3. Architecture

### 3.1 Object model

```
ROOT\ParsFlashVhci  (devnode created by pfdevcon)
 └─ WDFDEVICE  controller ................ PF_CONTROLLER_CONTEXT   Controller.cpp
     │   UdecxInitializeWdfDeviceInit + UdecxWdfDeviceAddUsbDeviceEmulation
     │   interfaces: GUID_DEVINTERFACE_USB_HOST_CONTROLLER (no reference string)
     │               PFVHCI_INTERFACE_GUID {5D1D9FCF-12B3-4DDF-BB2B-3B2E8EBB2584}, reference string "PFVHCI"
     │   default queue (parallel, passive)  -> IOCTL_PFVHCI_*, else UdecxWdfDeviceTryHandleUserIoctl
     │   FETCH queue (manual)               -> pended IOCTL_PFVHCI_FETCH
     │   completion DPC, record lookaside, spinlock, plug wait-lock
     ├─ WDFFILEOBJECT  (one per handle) .... PF_FILE_CONTEXT         owns devices + record FIFO
     └─ UDECXUSBDEVICE (one per plugged device) PF_DEVICE_CONTEXT    UsbDevice.cpp
         └─ UDECXUSBENDPOINT (EP0 + dynamic) PF_ENDPOINT_CONTEXT     Endpoint.cpp
             └─ WDFQUEUE  URB queue ........ PF_QUEUE_CONTEXT        (sequential for control, parallel otherwise)
WDFREQUEST (URB / UdeCx op) ............... PF_REQUEST_CONTEXT      RecordQueue.cpp (allocated on first use)
```

| File | Content |
|---|---|
| `Driver.cpp/.h` | `DriverEntry`, `EvtDriverDeviceAdd` |
| `Controller.cpp/.h` | WDFDEVICE creation, UdeCx config, interfaces, file objects, query-remove, registry |
| `UsbDevice.cpp/.h` | PLUGIN validation, `UDECXUSBDEVICE` creation/plug-in/unplug, ports, device callbacks |
| `Endpoint.cpp/.h` | endpoint creation, URB queues, URB → SUBMIT translation, purge/start/reset |
| `RecordQueue.cpp/.h` | inverted-call engine: record FIFO, FETCH filling, in-flight table, COMPLETE parsing, cancellation, completion DPC |
| `Ioctl.cpp/.h` | IOCTL dispatch and buffer validation |
| `Common.h` | includes, constants, contexts; `Trace.h` logging; `Version.h` + `pfvhci.rc` |

### 3.2 Locking and IRQL

- One `WDFSPINLOCK` per controller protects port table, device states, owner lists, per-file
  record FIFOs, the in-flight hash table (64 buckets keyed by `RequestId`), the completion
  list and request states. Only list manipulation happens under it; **data copies and request
  completion are always done with the lock released**.
- One `WDFWAITLOCK` (`PlugLock`, PASSIVE) serialises PLUGIN / UNPLUG / `EvtFileCleanup` /
  query-remove, i.e. everything that calls `UdecxUsbDevicePlugIn` / `PlugOutAndDelete`.
- Functions that take the spinlock are never pageable (`code_seg("PAGE")` only for code that is
  guaranteed PASSIVE_LEVEL and does not raise IRQL).
- URBs are completed at DISPATCH_LEVEL and **never on the submitter's thread** (UdeCx
  requirement): either via the completion DPC or from the driver's own IOCTL threads after
  `KeRaiseIrql(DISPATCH_LEVEL)`.

## 4. IOCTL interface

All IOCTLs are defined in `pfvhci_ioctl.h` (`FILE_DEVICE_UNKNOWN`, functions `0x900+`).
They are accepted **only on handles opened through the private interface** (device path from
`SetupDiGetDeviceInterfaceDetail(PFVHCI_INTERFACE_GUID)`, which ends in `\PFVHCI`).
Handles opened through `GUID_DEVINTERFACE_USB_HOST_CONTROLLER` get `STATUS_ACCESS_DENIED`
for them; every other IOCTL code is passed to `UdecxWdfDeviceTryHandleUserIoctl` (USBView etc.).

| IOCTL | Method | Input | Output | Notes |
|---|---|---|---|---|
| `GET_VERSION` | BUFFERED | — | `PFVHCI_VERSION_INFO` (32 B) | ABI, driver version, port counts |
| `PLUGIN` | BUFFERED | `PFVHCI_PLUGIN_HEADER` + descriptor entries | `PFVHCI_PLUGIN_RESULT` (8 B) | creates and plugs in a device owned by this handle |
| `UNPLUG` | BUFFERED | `PFVHCI_UNPLUG` | — | `DeviceHandle 0` = all devices of this handle; other owner → `STATUS_ACCESS_DENIED`; unknown → `STATUS_NOT_FOUND` |
| `FETCH` | OUT_DIRECT | — | record stream, **≥ `PFVHCI_MIN_FETCH_BUFFER` (64 KiB)** | pended until ≥ 1 record is available; smaller buffer → `STATUS_BUFFER_TOO_SMALL` |
| `COMPLETE` | IN_DIRECT | COMPLETION/OP_RESULT records | — | records in **either** `lpInBuffer` **or** `lpOutBuffer`, not both (see 4.2) |
| `GET_PORTS` | BUFFERED | — | `PFVHCI_PORT_LIST` | too small → `STATUS_BUFFER_OVERFLOW`, only `Count` written, `Information = 8` |

### 4.1 PLUGIN input validation (never trusts user mode)

The input is first copied to a private nonpaged buffer (kept for the device's lifetime) and
then validated completely before anything is created:

- `InputLength ≥ 24`, `≤ 1 MiB`, `Size == InputLength`, `AbiVersion == 1`,
  `Speed ∈ 1..5`, `Reserved0 == 0`, `1 ≤ EntryCount ≤ 1024`.
- Entries start at offset 24, each `8 + ALIGN8(Length)` bytes, `1 ≤ Length ≤ 64 KiB`, all
  inside the buffer, and the last one ends exactly at `Size` (no trailing bytes).
- Device descriptor: exactly one, 18 bytes, `bLength`/`bDescriptorType` consistent,
  `bMaxPacketSize0 ≠ 0`.
- Configuration descriptors: `bLength = 9`, type 2, `wTotalLength == Length`, every
  sub-descriptor `bLength ≥ 2` and inside the blob (endpoint descriptors `≥ 7`), no duplicate
  index, **indices `0..bNumConfigurations-1` all present**.
- String descriptors: `bLength == Length`, even, `≤ 255`, type 3; index 0 (LANGID list) must
  have `LangId 0`. String `0xEE` with the `MSFT100` signature yields the MS OS vendor code.
- Device qualifier: at most one, 10 bytes. BOS: at most one, `wTotalLength == Length`,
  capabilities `bLength ≥ 3`. **SuperSpeed devices must provide a BOS descriptor.**
- Any other descriptor type → `STATUS_INVALID_PARAMETER`.

Errors: `STATUS_BUFFER_TOO_SMALL`, `STATUS_INVALID_BUFFER_SIZE`, `STATUS_REVISION_MISMATCH`
(ABI), `STATUS_INVALID_PARAMETER`, `STATUS_INSUFFICIENT_RESOURCES` (also: no free port of the
required kind), `STATUS_DELETE_PENDING` (handle closing), or the UdeCx status.

### 4.2 COMPLETE buffer

The header documents the records as the *input* buffer. With `METHOD_IN_DIRECT` the I/O
manager **copies `lpInBuffer` into nonpaged pool** (buffered); only `lpOutBuffer` is described
by an MDL. Both are accepted:

- `lpInBuffer` (what the current client does): simple, but costs one copy and a nonpaged
  allocation of the whole buffer per call.
- `lpOutBuffer` with `lpInBuffer = NULL`: true direct I/O, recommended for large IN payloads.

The driver treats the buffer as shared with user mode: every header is copied to the stack
before validation and every write into a URB buffer is bounded by values the driver owns.
`Information` = bytes of records consumed. A structurally invalid record (bad `Size`, alignment,
`Flags`/`Reserved` ≠ 0, unknown type, wrong fixed size) stops parsing with
`STATUS_INVALID_PARAMETER`; records before it have been applied.

## 5. Record stream

Every record starts with `PFVHCI_RECORD_HEADER` (32 B) at an 8-byte aligned offset;
`Size` includes header and padding and is a multiple of 8. Padding bytes are zero.

| Type | Direction | Body | Completed by |
|---|---|---|---|
| `SUBMIT` 0x0001 | K→U | `PFVHCI_SUBMIT` (56 B) + `ISO_PACKET[IsoPacketCount]` + OUT data `[TransferLength]` (OUT only) | `COMPLETION` |
| `CANCEL` 0x0002 | K→U | `PFVHCI_ENDPOINT_OP`, `RequestId` = the cancelled SUBMIT, `Endpoint` = its endpoint | — |
| `ENDPOINT_RESET` 0x0003 | K→U | `ENDPOINT_OP.Endpoint` | `OP_RESULT` |
| `SELECT_CONFIG` 0x0004 | K→U | `ENDPOINT_OP.ConfigurationValue` (0 = unconfigure) | `OP_RESULT` |
| `SELECT_INTERFACE` 0x0005 | K→U | `ENDPOINT_OP.InterfaceNumber/AlternateSetting` | `OP_RESULT` |
| `DEVICE_RESET` 0x0006 | K→U | `ENDPOINT_OP` (no fields) | `OP_RESULT` |
| `DEVICE_REMOVED` 0x0007 | K→U | `ENDPOINT_OP` (no fields); only sent when the *controller* is being removed | — |
| `COMPLETION` 0x0081 | U→K | `PFVHCI_COMPLETION` (48 B) + `ISO_RESULT[IsoPacketCount]` + IN data `[ActualLength]` (IN only) | — |
| `OP_RESULT` 0x0082 | U→K | `PFVHCI_OP_RESULT` (40 B) | — |

**COMPLETION size rule (strict):** `Size == ALIGN8(48 + 8·IsoPacketCount + (IN ? ActualLength : 0))`.
For non-isochronous transfers `IsoPacketCount` must be 0. For isochronous transfers it is
either 0 (no per-packet results; only allowed without IN data — every packet gets the overall
status) or exactly the SUBMIT's `IsoPacketCount`.

**Isochronous IN data layout (driver convention — not specified by the ABI header):** the data
area is *packed*: packet *i* contributes exactly `Iso[i].ActualLength` bytes, in packet order;
`ActualLength == Σ Iso[i].ActualLength`. The driver scatters each packet to its `Offset` from the
SUBMIT. `Iso[i].ActualLength` must not exceed the packet length. This is the USB/IP convention.

### 5.1 FETCH: batching and the overflow retry rule

- A FETCH is completed as soon as at least one record is queued; it is filled with **as many
  whole records as fit** (batching under load, single record under light load).
- If the **first** queued record does not fit into an otherwise empty buffer, the driver writes
  only its `PFVHCI_RECORD_HEADER` (with `Size` = bytes needed) and completes with
  `STATUS_BUFFER_OVERFLOW` (`ERROR_MORE_DATA` in user mode), `Information = 32`. The record
  **stays queued**; user mode re-issues FETCH with a buffer of at least `Size` bytes
  (largest possible record ≈ 56 + 8·1024 + 16 MiB).
- OUT payloads are copied straight from the URB buffer into the FETCH MDL (no intermediate copy).

### 5.2 Ordering

- Records are delivered in the order they were queued (FIFO per handle). Only one thread fills
  FETCH requests of a handle at a time, so FETCH requests are completed in record order.
- `RequestId` is a per-controller counter assigned when a record is queued: **strictly
  increasing in queue order**. With several fetch threads, user mode must use `RequestId` to
  restore per-endpoint submission order (threads may wake up in any order).
- A `CANCEL` for a request is always queued after its `SUBMIT` was delivered.

## 6. User/kernel communication (inverted call)

```
function driver ─URB─► UdeCx ─► endpoint WDFQUEUE ─► EvtIoInternalDeviceControl
                                                       │ translate URB → SUBMIT, mark cancelable
                                                       ▼
                                       owner file's record FIFO ──► pended FETCH (manual queue)
                                                                       │ copy (OUT data from URB)
                                                                       ▼
                                                 in-flight table (RequestId → WDFREQUEST)
user mode ──IOCTL_PFVHCI_COMPLETE──► lookup, copy IN data into URB, map status ──► UdecxUrbComplete
```

### 6.1 Ownership by file object

- Every `UDECXUSBDEVICE` is **owned by the handle (file object) that plugged it in**. Only that
  handle receives its records, may complete its requests and may unplug it.
- `EvtFileCleanup` (last handle closed, including process crash) unplugs every device the
  handle owns; their queued and in-flight URBs complete with
  `STATUS_DEVICE_NOT_CONNECTED` / `USBD_STATUS_DEVICE_GONE`; pended FETCHes are cancelled.
  No orphaned devices, no hung applications.

### 6.2 Cancellation

URB requests stay cancelable (`WdfRequestMarkCancelableEx`) from queuing until completion.
All state changes happen under the spinlock; `WdfRequestUnmarkCancelable` is called with the
lock held (it never waits for the cancel routine, so this cannot deadlock) and, if it reports
`STATUS_CANCELLED`, the cancel routine completes the request.

| Cancelled while… | Result |
|---|---|
| queued (not fetched) | removed from the FIFO, URB completes `STATUS_CANCELLED` / `USBD_STATUS_CANCELED`; user mode never sees it |
| being copied into a FETCH | flagged; when the FETCH path finishes it completes the URB as cancelled and queues a `CANCEL` record |
| in flight (fetched) | removed from the in-flight table, URB completes cancelled, `CANCEL` record queued |
| COMPLETE racing with cancel | whichever takes the request out of the table first wins; the loser does nothing |

A **late `COMPLETION`/`OP_RESULT`** (unknown `RequestId`, already cancelled) is silently
ignored (traced). User mode must also tolerate a `CANCEL` for a request it has just completed.

Endpoint purge (`EvtUsbEndpointPurge`, e.g. pipe abort, configuration change, unplug):
`WdfIoQueuePurge` first (no new dispatches; undispatched requests → `EvtIoCanceledOnQueue`),
then every queued/in-flight request of that endpoint is cancelled as above, and
`UdecxUsbEndpointPurgeComplete` is called from the purge-complete callback once WDF has seen all
requests delivered from that queue complete. An endpoint "abort sequence" counter catches
requests that were being copied into a FETCH at that moment.

## 7. USB request handling

UdeCx itself answers standard requests for which descriptors were provided (GET_DESCRIPTOR for
device/configuration/string/BOS, SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE,
CLEAR_FEATURE(ENDPOINT_HALT), SET_SEL, …), and the USB stack above UdeCx normally converts
request-style URBs into control transfers before they reach the endpoint queue. All URB
functions a function driver can send are nevertheless translated:

| URB function | Record | Details |
|---|---|---|
| `CONTROL_TRANSFER`, `CONTROL_TRANSFER_EX` | SUBMIT, control | setup copied; direction = `bmRequestType` bit 7; data length = min(`TransferBufferLength`, `wLength`), `wLength` lowered if the buffer is smaller; `_EX` timeout → `TimeoutMs` |
| `BULK_OR_INTERRUPT_TRANSFER` (+`_USING_CHAINED_MDL`) | SUBMIT, bulk/interrupt | direction from endpoint address; endpoint type must match |
| `ISOCH_TRANSFER` (+`_USING_CHAINED_MDL`) | SUBMIT, isochronous | 1..1024 packets, offsets non-decreasing and inside the buffer, packet length = next offset − offset; `StartFrame` not forwarded (ASAP) |
| `GET_DESCRIPTOR_FROM_DEVICE/INTERFACE/ENDPOINT` | SUBMIT, control | `0x80/81/82, 6, (type<<8)|index, LanguageId` |
| `SET_DESCRIPTOR_TO_DEVICE/INTERFACE/ENDPOINT` | SUBMIT, control | `0x00/01/02, 7`, OUT |
| `SET_FEATURE_TO_*` / `CLEAR_FEATURE_TO_*` (device/interface/endpoint/other) | SUBMIT, control | `recipient, 3/1, FeatureSelector, Index`, no data |
| `GET_STATUS_FROM_*` | SUBMIT, control | `0x80|recipient, 0, 0, Index` |
| `VENDOR_*` / `CLASS_*` (device/interface/endpoint/other) | SUBMIT, control | direction from `USBD_TRANSFER_DIRECTION_IN`, type vendor/class, `RequestTypeReservedBits` OR-ed in |
| `GET_CONFIGURATION` | SUBMIT, control | `0x80, 8` |
| `GET_INTERFACE` | SUBMIT, control | `0x81, 10, 0, Interface` |
| `GET_MS_FEATURE_DESCRIPTOR` | SUBMIT, control | vendor IN request with `bMS_VendorCode` of string 0xEE, `wValue = (InterfaceNumber<<8)|PageIndex`, `wIndex = FeatureIndex`; without 0xEE string → `USBD_STATUS_STALL_PID` |
| `SYNC_RESET_PIPE_AND_CLEAR_STALL`, `SYNC_RESET_PIPE`, `SYNC_CLEAR_STALL` | ENDPOINT_RESET | target endpoint found by learned pipe handle; URB completes on OP_RESULT |
| `ABORT_PIPE` | (CANCEL records) | cancels all requests of the target endpoint, completes successfully |
| anything else (incl. `SELECT_CONFIGURATION`, which UdeCx turns into EndpointsConfigure) | — | `STATUS_NOT_SUPPORTED` / `USBD_STATUS_INVALID_URB_FUNCTION` |

Transfer flags: `DIR_IN` is always set from the direction. `SHORT_NOT_OK` is never set (the
Windows xHCI stack treats short IN transfers as success regardless of
`USBD_SHORT_TRANSFER_OK`); `ZERO_PACKET` is never set (Windows has no URB flag for it).
Limits: transfers > `PFVHCI_MAX_TRANSFER_BYTES` (16 MiB) fail with `USBD_STATUS_INVALID_PARAMETER`.

UdeCx operations:

| UdeCx callback | Record / action |
|---|---|
| `EvtUsbEndpointReset` | ENDPOINT_RESET, request completed with the OP_RESULT status |
| `EvtUsbDeviceEndpointsConfigure` — `DeviceInitialize`, `EndpointsReleasedOnly` | completed locally (success) |
| — `DeviceConfigurationChange` | SELECT_CONFIG, completed on OP_RESULT |
| — `InterfaceSettingChange` | SELECT_INTERFACE, completed on OP_RESULT |
| `EvtUsbDeviceReset` (post-enumeration port reset) | DEVICE_RESET, completed on OP_RESULT |
| `EvtUsbEndpointStart` / `Purge` | `WdfIoQueueStart` / see §6.2 |
| `EvtUsbDeviceLinkPowerEntry/Exit`, `SetFunctionSuspendAndWake` | `STATUS_SUCCESS` |

Status mapping (`COMPLETION.Status`, per-packet `ISO_RESULT.Status`):

| `PFVHCI_STATUS_*` | URB: `USBD_STATUS` | URB completion | OP_RESULT on non-URB request: NTSTATUS |
|---|---|---|---|
| SUCCESS | `SUCCESS` | `UdecxUrbComplete` | `STATUS_SUCCESS` |
| STALL | `STALL_PID` | `UdecxUrbComplete` | `STATUS_UNSUCCESSFUL` |
| TIMEOUT | `TIMEOUT` | `UdecxUrbComplete` | `STATUS_IO_TIMEOUT` |
| CANCELLED | `CANCELED` | `…WithNtStatus(STATUS_CANCELLED)` | `STATUS_CANCELLED` |
| OVERFLOW | `BABBLE_DETECTED` | `UdecxUrbComplete` | `STATUS_DATA_OVERRUN` |
| SHORT_PACKET | `ERROR_SHORT_TRANSFER` | `UdecxUrbComplete` | `STATUS_UNSUCCESSFUL` |
| DEVICE_GONE, NO_DEVICE | `DEVICE_GONE` | `…WithNtStatus(STATUS_DEVICE_NOT_CONNECTED)` | `STATUS_DEVICE_NOT_CONNECTED` |
| INVALID | `INVALID_PARAMETER` | `UdecxUrbComplete` | `STATUS_INVALID_PARAMETER` |
| NOT_SUPPORTED | `NOT_SUPPORTED` | `UdecxUrbComplete` | `STATUS_NOT_SUPPORTED` |
| IO_ERROR | `DEV_NOT_RESPONDING` | `UdecxUrbComplete` | `STATUS_IO_DEVICE_ERROR` |
| other | `REQUEST_FAILED` | `UdecxUrbComplete` | `STATUS_UNSUCCESSFUL` |

IN data longer than the URB buffer is truncated and a successful result becomes
`USBD_STATUS_BABBLE_DETECTED`. A COMPLETION that is inconsistent with its request (size rule,
iso counts, packet overrun) completes the URB with `USBD_STATUS_INVALID_PARAMETER`.

## 8. Device enumeration and descriptors

1. `IOCTL_PFVHCI_PLUGIN` → validation (§4.1) → free port of the right kind
   (`SUPER`/`SUPER_PLUS` → USB 3 port, otherwise USB 2 port).
2. `UdecxUsbDeviceInitAllocate`, state-change callbacks, `UdecxUsbDeviceInitSetSpeed`
   (low/full/high/super; SuperSpeedPlus is reported as SuperSpeed), dynamic endpoints.
3. Descriptors: device, BOS, qualifier → `UdecxUsbDeviceInitAddDescriptor`; configuration *n*
   → `…AddDescriptorWithIndex(n)`; LANGID list → `…AddDescriptorWithIndex(0)`; other strings →
   `UdecxUsbDeviceInitAddStringDescriptor(UNICODE_STRING, index, langid)`.
4. `UdecxUsbDeviceCreate`, publish (handle, port table, owner list, state *PluggedIn*),
   `UdecxUsbDevicePlugIn` with `Usb20PortNumber`/`Usb30PortNumber`.
5. Root hub port numbering: USB 2 ports `1..N2`, USB 3 ports `N2+1..N2+N3` (global numbers,
   also reported in `PFVHCI_PLUGIN_RESULT.PortNumber` and `GET_PORTS`).
6. usbhub3 enumerates; UdeCx calls `EvtUsbDeviceDefaultEndpointAdd` (EP0), later
   `EvtUsbDeviceEndpointAdd` and `EvtUsbDeviceEndpointsConfigure` for SET_CONFIGURATION /
   SET_INTERFACE.

`GET_PORTS` reports, for each port, the device handle (0 = empty), the cookie (only for devices
owned by the calling handle, otherwise 0) and the speed; an empty port reports the port's type
(`HIGH` for USB 2 ports, `SUPER` for USB 3 ports).

Port counts come from `HKLM\SYSTEM\CurrentControlSet\Services\pfvhci\Parameters`
(`NumberOfUsb20Ports`, `NumberOfUsb30Ports`, REG_DWORD, default 8, clamped to 1..127), read at
controller start.

## 9. PnP lifecycle

| Event | What happens |
|---|---|
| Install | `pfdevcon install pfvhci.inf`: create `ROOT\USB\NNNN` with hardware ID `Root\ParsFlashVhci`, `DIF_REGISTERDEVICE`, `UpdateDriverForPlugAndPlayDevices(INSTALLFLAG_FORCE)` |
| Start | `EvtDriverDeviceAdd` → controller ready, interfaces enabled; no devices |
| Plug-in / unplug | `IOCTL_PFVHCI_PLUGIN` / `IOCTL_PFVHCI_UNPLUG` (§8) |
| Owner handle closed / client crash | `EvtFileCleanup` unplugs the handle's devices (§6.1) |
| Disable / uninstall / upgrade | `EvtDeviceQueryRemove` unplugs **all** devices (owners get `DEVICE_REMOVED`) before the stack is torn down. If PnP cancels the removal, user mode simply plugs devices in again. The controller cannot be removed while handles are open: the client must close its handle when it sees `DEVICE_REMOVED` or the interface-removal notification. |
| Surprise removal of the controller (rare for a root device) | UdeCx tears down the children; the port table references are dropped in the controller cleanup callback. |
| Driver unload | after the last controller is removed; WDF deletes all objects. |
| Upgrade | `pfdevcon update new\pfvhci.inf` → PnP stops the old instance (query-remove path above) and starts the new one; registry parameters survive (`FLG_ADDREG_NOCLOBBER`). |

Unplug sequence (UNPLUG, cleanup, query-remove): device state → *Gone* (new URBs fail
immediately), removed from tables, queued/in-flight requests completed with DEVICE_GONE
**before** `UdecxUsbDevicePlugOutAndDelete`, then the table's WDF reference is dropped.

## 10. Power management

- The controller is virtual: no S0 idle, all queues are **not power-managed**.
- Per-device link power callbacks (`EvtUsbDeviceLinkPowerEntry/Exit`,
  `EvtUsbDeviceSetFunctionSuspendAndWake` for SuperSpeed devices) accept every transition. The
  remote device is not suspended; selective suspend of the virtual device is therefore cheap.
- Remote wake is not supported (no `UdecxUsbDeviceSignalWake` calls).
- System sleep: the network connection usually breaks; the client's session logic
  (ARCHITECTURE §2.6) decides whether devices are kept or unplugged after resume.

## 11. Error recovery

| Failure | Handling |
|---|---|
| Client crash / handle closed | §6.1 |
| Malformed PLUGIN | rejected before anything is created |
| Malformed COMPLETE record | parsing stops with `STATUS_INVALID_PARAMETER`; inconsistent record for a valid request → that URB fails with `USBD_STATUS_INVALID_PARAMETER` |
| Late completion | ignored (§6.2) |
| Remote device gone | user mode completes with `PFVHCI_STATUS_DEVICE_GONE` and/or unplugs |
| User mode stops fetching | URBs stay pending (applications see a stalled device until they cancel); informational records are capped at 4096 per handle (further ones dropped and traced) |
| Out of memory | request context allocation failure → URB fails `STATUS_INSUFFICIENT_RESOURCES`; missing CANCEL record → traced, late completion is ignored |
| FETCH buffer too small for one record | overflow retry rule (§5.1) |

## 12. Security

- **Device ACL** (INF `.HW` `Security` and `WdfDeviceInitAssignSDDLString` on the FDO):
  `D:P(A;;GA;;;SY)(A;;GA;;;BA)` — **SYSTEM and Administrators only**. Whoever can open the
  controller can make arbitrary USB devices (keyboards, network adapters, storage) appear on the
  machine, which is equivalent to physical access; it must not be available to standard users.
- Consequence for the product: the client engine that talks to the driver must run as
  **LocalSystem (Windows service)** or **elevated**. The GUI should talk to a client service
  rather than open the driver itself. To grant a dedicated virtual service account instead of
  LocalSystem, append `(A;;GRGW;;;S-1-5-80-…)` (the service SID from `sc showsid <service>`) to
  the SDDL in the INF.
- Private IOCTLs only on handles opened through the private interface; standard host-controller
  handles cannot plug devices.
- Devices are owned by the handle that plugged them (no cross-handle unplug / completion).
- All user input is validated against `PFVHCI_MAX_*` bounds and the actual buffer sizes; shared
  (direct I/O) buffers are read once into kernel copies; padding in FETCH buffers is zeroed.
- Signed by ParsFlash only (`PnpLockdown=1`, driver package isolation via DIRID 13).

## 13. Building (VS 2022 + WDK)

1. Install Visual Studio 2022 with *Desktop development with C++* and the
   *MSVC v143 x64 Spectre-mitigated libraries*; install Windows SDK + WDK **10.0.22621** or newer
   (and the WDK VS extension).
2. Open `Driver/Windows/KMDF/pfvhci/pfvhci.sln`, select `Release|x64`, build; or
   `msbuild pfvhci.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PfvhciBuild=<n>`.
3. Project settings: KMDF 1.23, `UdeDriver=true`, UDE 1.1, `TargetVersion=Windows10`,
   `/W4 /WX`, C++17, Spectre mitigation, include `../../../../Shared/Driver`, links `wdmsec.lib`
   (+ `udecxstub.lib` via `UdeDriver`). Output: `<repo>/out/driver/x64/<cfg>/` (git-ignored):
   `pfvhci.sys`, `pfvhci.pdb`, and the package folder with `pfvhci.inf` (stamped) and
   `pfvhci.cat` (Inf2Cat).
4. Run Code Analysis (`/analyze`) and Static Driver Verifier before every release.

`pfdevcon`: `add_subdirectory(Driver/Windows/pfdevcon)` from the top-level CMake project (x64
MSVC or MinGW-w64). Manual MinGW build:
`x86_64-w64-mingw32-g++-posix -std=c++20 -Wall -Wextra -municode -IShared/Driver Driver/Windows/pfdevcon/pfdevcon.cpp -lsetupapi -lnewdev -lcfgmgr32 -static -o pfdevcon.exe`.

## 14. Versioning, signing, install

### 14.1 Versioning

- `Version.h` holds major.minor.patch (1.0.0); the build number comes from CI
  (`/p:PfvhciBuild=<n>` → `PFVHCI_VERSION_BUILD` for C++ and RC).
- stampinf writes `DriverVer = <build date>,1.0.0.<n>` and `$KMDFVERSION$` into the packaged INF.
  The file version resource and `IOCTL_PFVHCI_GET_VERSION` report the same 1.0.0.<n>.
- `PFVHCI_ABI_VERSION` is independent; the client refuses to run against a different ABI.
- Windows picks the driver with the newest `DriverVer` date/version — always bump the build number.

### 14.2 Test signing (development machines only)

```bat
:: on the test machine (Secure Boot must be off for test signing)
bcdedit /set testsigning on
shutdown /r /t 0

:: on the build machine (elevated PowerShell): create a test certificate
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=ParsFlash Test" `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable -HashAlgorithm sha256
Export-Certificate -Cert $cert -FilePath ParsFlashTest.cer
:: (the Debug configuration does this automatically with the WDK test certificate: SignMode=TestSign)

:: catalog + signatures (if not done by the build)
inf2cat /driver:out\driver\x64\Debug\pfvhci /os:10_RS3_X64,10_X64,Server10_X64
signtool sign /fd sha256 /s My /n "ParsFlash Test" /t http://timestamp.digicert.com out\driver\x64\Debug\pfvhci\pfvhci.sys
signtool sign /fd sha256 /s My /n "ParsFlash Test" /t http://timestamp.digicert.com out\driver\x64\Debug\pfvhci\pfvhci.cat

:: on the test machine: trust the certificate, then install
certutil -addstore Root ParsFlashTest.cer
certutil -addstore TrustedPublisher ParsFlashTest.cer
pnputil /add-driver pfvhci.inf          :: optional: stage only
pfdevcon install pfvhci.inf
pfdevcon status
```

### 14.3 Production signing

- Windows 10 1607+ and Windows 11 load only Microsoft-signed kernel drivers (Secure Boot on).
- Process: build Release → sign `pfvhci.sys` and `pfvhci.cat` with the company **EV code
  signing certificate** → package (`.cab` with `makecab`, signed with the EV cert) → submit to
  **Microsoft Partner Center (Hardware Dev Center) for attestation signing** (Windows 10/11
  client) → ship the Microsoft-signed package returned.
- For a WHQL / "Windows compatible" listing (and Windows Server), run the **HLK** tests for the
  device class and submit the HLK package instead of attestation.
- Run `infverif /w` on the INF before submission.

### 14.4 Install / uninstall / upgrade (installer)

| Action | Command | Notes |
|---|---|---|
| install | `pfdevcon install <dir>\pfvhci.inf` | idempotent: reuses an existing present node; exit 3010 → reboot required |
| upgrade | `pfdevcon update <dir>\pfvhci.inf` | fails if not installed |
| uninstall | `pfdevcon remove` | removes all nodes (also non-present ones) and the `oemNN.inf` package(s) via `DiUninstallDriverW` (fallback `SetupUninstallOEMInfW(SUOI_FORCEDELETE)`) |
| check | `pfdevcon status` | exit 0 only if the node is started and `IOCTL_PFVHCI_GET_VERSION` succeeds with the same ABI (needs elevation because of the device ACL) |

All commands except `status` require an elevated 64-bit process. Exit codes: 0 ok, 1 error,
3010 reboot required. Close the ParsFlash client before `update`/`remove` (open handles block
the controller removal).

## 15. Debugging

- **Kernel debugging**: KDNET over the network —
  `bcdedit /debug on` + `bcdedit /dbgsettings net hostip:<host> port:50000 key:<k>`;
  WinDbg: `windbg -k net:port=50000,key=<k>`.
- **Logging**: `DbgPrintEx(DPFLTR_IHVDRIVER_ID)`. Enable with
  `reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v IHVDRIVER /t REG_DWORD /d 0xF`
  (reboot) or `ed nt!Kd_IHVDRIVER_Mask 0xF` in WinDbg; view in WinDbg or DebugView. Per-URB
  traces exist only in Debug builds.
- **WDF**: `!wdfkd.wdfldr`, `!wdfkd.wdfdriverinfo pfvhci`, `!wdfkd.wdfdevice`, `!wdfkd.wdfqueue`,
  `!wdfkd.wdfrequest`, `!wdfkd.wdflogdump pfvhci` (IFR log). Enable the KMDF verifier:
  `HKLM\SYSTEM\CurrentControlSet\Services\pfvhci\Parameters\Wdf` →
  `VerifierOn=1`, `VerboseOn=1`, `VerifyDownLevel=1`, `TrackHandles="*"`, `DbgBreakOnError=1`.
- **USB**: USBView (shows the virtual controller), `!usbkd` extension, ETW provider
  `Microsoft-Windows-USB-UCX` / `USB-USBHUB3` traces (`logman`/`netsh trace`, analysed with
  Message Analyzer or the USB ETW parsers) to see what UdeCx and usbhub3 do.
- **Driver Verifier (mandatory before any release)**:
  `verifier /standard /driver pfvhci.sys udecx.sys Wdf01000.sys` (= flags `0x209BB`: special
  pool, force IRQL checking, pool tracking, I/O verification, deadlock detection, DMA checking,
  security checks, miscellaneous checks, DDI compliance checking), reboot, run the stress tests
  below. Then a separate pass with randomized low-resources simulation
  (`verifier /faults /driver pfvhci.sys`) to exercise every allocation-failure path.
  Pool tag: `Pfvh`.
- Stress: plug/unplug loops, kill the client process with URBs in flight, cancel storms
  (`adb` killed during transfers), FETCH with tiny/huge buffers, malformed COMPLETE fuzzing.

## 16. Known limitations (v1)

- No remote wake, no streams (SuperSpeed bulk streams are reported unsupported by UdeCx).
- No chained-MDL support (`GUID_USB_CAPABILITY_CHAINED_MDLS` not supported).
- Isochronous `StartFrame` is not forwarded (ASAP scheduling only).
- One spinlock per controller; fine for tens of devices, revisit if profiling shows contention.

## 17. Open VERIFY items

Every item below is unverified and must be checked on the first WDK build / test run.

1. `EVT_UDECX_USB_DEVICE_POST_ENUMERATION_RESET` parameter list
   (`WDFDEVICE, UDECXUSBDEVICE, WDFREQUEST, BOOLEAN AllDevicesReset`, taken from usbip-win2; not
   documented on Microsoft Learn). Mismatch → compile error at the callback assignment.
2. Composite devices: usbip-win2 reports that UdeCx does **not** call
   `EvtUsbDeviceEndpointsConfigure(DeviceConfigurationChange)` for composite devices and passes
   wrong `InterfaceNumber`/`NewInterfaceSetting` for `InterfaceSettingChange`; usbip-win2 uses an
   upper filter driver to intercept SELECT_CONFIGURATION/SELECT_INTERFACE. If confirmed (test an
   ADB+MTP phone early), SELECT_CONFIG/SELECT_INTERFACE records will be missing/wrong and a filter
   driver must be added.
3. Root hub port numbering passed to `UdecxUsbDevicePlugIn` (global numbering
   USB 2 = 1..N2, USB 3 = N2+1.., as in usbip-win2).
4. `UdecxUrbCompleteWithNtStatus` and `Urb->UrbHeader.Status`; usbip-win2 reports that error
   completions after `UdecxUsbDevicePlugOutAndDelete` can keep UdeCx from deleting the device.
   DEVICE_GONE completions are done before plug-out, but late cancels can race with it.
5. Descriptor APIs: `UdecxUsbDeviceInitAddDescriptorWithIndex` for multiple configurations,
   `UdecxUsbDeviceInitAddDescriptor` for the device qualifier, whether UdeCx copies the buffers
   (the driver keeps its own copy alive anyway), and that `UdecxUsbDeviceInitAddStringDescriptor`
   returns `bString` byte-for-byte (including MS OS string 0xEE requested with LANGID 0).
6. `UdecxWdfDeviceResetComplete(WDFDEVICE)` signature; behaviour of
   `ResetAction = UdecxWdfDeviceResetActionResetEachUsbDevice`.
7. `_URB_OS_FEATURE_DESCRIPTOR_REQUEST` member names (`Recipient`, `InterfaceNumber`,
   `MS_PageIndex`, `MS_FeatureDescriptorIndex`).
8. `GUID_USB_CAPABILITY_*` declared by `usbdlib.h` (else include `usbbusif.h`).
9. `WdfRequestRetrieveOutputBuffer` maps the `METHOD_IN_DIRECT` buffer (COMPLETE via `lpOutBuffer`).
10. `WdfExecutionLevelPassive` accepted on the controller's default queue.
11. vcxproj: `UdeDriver=true` adds the UdeCx headers and `udecxstub.lib`; property/metadata names
    for Inf2Cat (`EnableInf2cat`, `Inf2Cat.UseLocalTime`) and stampinf (`Inf.TimeStamp`,
    `SpecifyDriverVerDirectiveVersion`); KMDF 1.23 with WDK 22621+.
12. INF: DIRID 13 accepted on Windows 10 1709; `Dependencies = ucx01000,udecx`;
    `infverif /w` clean.
13. `EvtUsbDeviceSetFunctionSuspendAndWake` registered only for SuperSpeed devices (as usbip-win2).
14. Behaviour of the hub when a SuperSpeed device's BOS/descriptors are inconsistent with
    `UdecxUsbDeviceInitSetSpeed` (e.g. SUPER_PLUS reported as SuperSpeed).
