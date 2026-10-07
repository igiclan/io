# ParsFlash USB Redirector — Architecture

Status legend used throughout the docs:

| Tag | Meaning |
|-----|---------|
| **BUILT+TESTED** | Compiled and covered by automated tests that pass in CI/dev environment |
| **BUILT** | Compiles; not (yet) exercised by automated tests on real hardware |
| **UNBUILT** | Source exists but could not be compiled in the development environment (e.g. needs WDK) |
| **NOT IMPLEMENTED** | Designed, not written |

The authoritative per-component status table is in [README.md](../README.md#implementation-status).

---

## 1. Goal

Redirect a *physical* USB device attached to a Windows **Server** PC to a Windows
**Client** PC so that unmodified Windows software on the Client (ADB, fastboot,
QFIL/EDL tools, SP Flash Tool, Odin, Huawei/Xiaomi tools, …) talks to it through
the normal Windows USB stack — i.e. the device appears in Device Manager on the
Client, its normal function driver loads, and URBs flow end-to-end.

```
 Physical USB device
        │  (USB cable)
 ┌──────▼─────────────────────────── Server PC ───────────────────────────────┐
 │ UsbDk filter (captures device)   ◄── libusb-1.0 (UsbDk backend)            │
 │ ParsFlash USB Server: DeviceMonitor · UsbBackend · SessionManager ·        │
 │                       ProtocolEngine · TLS · REST API · Diagnostics         │
 └──────┬─────────────────────────────────────────────────────────────────────┘
        │  PFUP/1 binary protocol over TLS 1.3 / TCP (default port 39000)
 ┌──────▼─────────────────────────── Client PC ───────────────────────────────┐
 │ ParsFlash USB Client: ProtocolEngine · SessionManager · VhciBridge          │
 │        │  DeviceIoControl (inverted-call, batched)                          │
 │ pfvhci.sys  (KMDF + UdeCx virtual host controller, root-enumerated)         │
 │        │  UdeCx → usbhub3 / usbccgp → function drivers (WinUSB, usbser…)    │
 │ Repair software (adb.exe, fastboot.exe, QFIL, SP Flash Tool, Odin, …)       │
 └─────────────────────────────────────────────────────────────────────────────┘
```

## 2. Key decisions and rationale

### 2.1 Client: how the remote device is exposed to Windows

Windows only loads USB function drivers for devices that a **USB host controller
driver** reports on a **USB hub**. A user-mode program cannot do that; a kernel
driver is mandatory.

Options considered:

| Option | Verdict |
|---|---|
| Raw WDM bus driver that fakes a hub and answers hub IOCTLs (classic usbip-win "vhci") | Rejected: undocumented contracts with usbccgp/usbhub, breaks across Windows builds. |
| **USB Device Emulation (UDE): KMDF + UdeCx class extension** | **Chosen.** Microsoft-supported (Windows 10 1607+, `udecx.sys` inbox), the OS supplies the real `usbhub3` stack on top of our emulated host controller; we only implement the controller/device/endpoint callbacks. Same approach as the actively maintained usbip-win2 project. |
| USB/IP Linux vhci-style protocol in kernel (WSK sockets in driver) | Rejected for v1: puts TLS + auth in the kernel. |

Consequences:
* Driver `pfvhci.sys` is a **root-enumerated KMDF driver** (`ROOT\ParsFlashVhci`) that
  calls `UdecxWdfDeviceAddUsbDeviceEmulation` and exposes
  `GUID_DEVINTERFACE_USB_HOST_CONTROLLER` plus a private interface
  `GUID_DEVINTERFACE_PFVHCI`.
* Each attached remote device is a `UDECXUSBDEVICE` created from the **real
  descriptors** fetched from the server (device, configuration, BOS, strings),
  plugged into a free USB 2.0 or USB 3.x port of the virtual controller.
* Endpoints are created dynamically (`UdecxEndpointTypeDynamic`); each endpoint has
  a WDF queue; every URB arriving on a queue is **forwarded to user mode** (see 2.3).
* Windows 10 x64 1709+ and Windows 11 x64 are supported (UdeCx 1.1 needed for
  `NumberOfUsb20Ports/NumberOfUsb30Ports`). Windows 10 builds earlier than 1709 are
  NOT supported.

Details: [DRIVER.md](DRIVER.md).

### 2.2 Server: how the physical device is captured

The Server must own the whole device (all interfaces, EP0) so that it can replay
arbitrary URBs, while the device's normal Windows driver on the Server must *not*
use it concurrently.

| Option | Verdict |
|---|---|
| WinUSB per device (requires replacing the driver, e.g. via Zadig) | Supported as a fallback (libusb WinUSB backend) but cumbersome for phones that re-enumerate with new VID/PIDs on every mode switch. |
| Own server-side KMDF stub/filter driver | Best long-term; NOT IMPLEMENTED in v1 (would double the signed-driver surface). |
| **UsbDk** (Red Hat, Apache-2.0, signed, used by SPICE/virt-viewer for exactly this purpose) driven through **libusb-1.0** | **Chosen for v1.** UsbDk is a filter driver that detaches a device from its function driver *on demand* while it is redirected and gives it back afterwards; works for any class, composite devices, and survives re-enumeration (new PIDs are captured on open). |

The Server's USB layer is an interface (`IUsbBackend`) with two implementations:

* `LibusbBackend` — production backend. Windows: libusb + UsbDk (`LIBUSB_OPTION_USE_USBDK`),
  falls back to WinUSB-bound devices. Linux: libusb/usbfs (a Linux server is a free by-product).
* `LoopbackUsbBackend` — a **test double** emulating a vendor-class device with bulk
  echo endpoints and scripted re-enumeration (ADB → bootloader → fastboot). It exists
  only for automated tests/benchmarks and is never shown as a real device unless the
  server is started with `--test-devices`.

Rich topology information (physical location path, parent hub, port number,
container ID, driver) comes from a platform `DeviceInventory`:
Windows = SetupAPI/CfgMgr32 + hub IOCTLs, Linux = sysfs. It is merged with libusb
data by bus/port path.

### 2.3 Client user/kernel communication: inverted call, batched

```
 pfvhci.sys                                   ParsFlash Client (user mode)
 ┌──────────────────────┐   IOCTL_PFVHCI_FETCH (pended, METHOD_OUT_DIRECT)
 │ endpoint queues      │◄────────────────────── N worker threads keep ≥2 pended
 │  URB → record queue  │──► fills buffer with 1..k SUBMIT/CANCEL records ──►
 │                      │   IOCTL_PFVHCI_COMPLETE (METHOD_IN_DIRECT)
 │ in-flight table      │◄────────────────────── 1..k COMPLETION records (+IN data)
 └──────────────────────┘
```
* No polling: the driver completes a pended FETCH as soon as a URB arrives and
  packs as many records as fit (batching under load, immediate under light load).
* OUT payloads are copied into the FETCH buffer; IN payloads ride in the COMPLETE
  buffer. Direct I/O (MDL) avoids an extra kernel copy.
* URB cancellation (e.g. `adb` killed, `IoCancelIrp`) produces a CANCEL record that
  the client turns into a `USB_CANCEL` message.
* Every device is owned by the **file object** that plugged it; if the client
  process crashes, `EvtFileCleanup` unplugs its devices and fails pending URBs with
  `STATUS_DEVICE_NOT_CONNECTED` — no orphaned devices, no hung applications.

### 2.4 Transport and protocol

* **PFUP/1** ("ParsFlash USB Protocol", version 1). Fixed 16-byte little-endian frame
  header, typed messages. Control plane messages (HELLO, AUTH, DEVICE_LIST, …)
  carry compact JSON; the **data plane (USB_SUBMIT/USB_COMPLETE/USB_CANCEL) is
  binary**. Spec: [PROTOCOL.md](PROTOCOL.md).
* Transport abstraction `ITransport` (byte stream). v1: TCP (+ optional TLS 1.3 via
  OpenSSL). Future: QUIC/UDP, named pipes — only the transport factory changes.
* Asynchronous I/O: **Asio** (standalone). On Windows Asio is implemented on
  **IOCP + overlapped I/O**; on Linux on epoll. We rely on it rather than a
  hand-rolled IOCP loop: it is mature and gives the same semantics on both platforms,
  which is what allows the full stack to be tested in CI on Linux *and* under Wine.
* Send path: per-connection write queue with **gather writes** (several frames per
  `WSASend`), frames built in pooled buffers, **byte-based backpressure** (high/low
  watermarks) so a slow link cannot grow memory without bound.
* `TCP_NODELAY` on (latency of EP0/handshake-style traffic dominates in repair
  tools); SO_KEEPALIVE on + application PING/PONG (keepalive interval configurable,
  idle traffic ~1 small frame per interval).

### 2.5 Concurrency model

* One Asio `io_context` per process with `worker_threads` threads (default:
  min(4, cores)). Each connection is serialized by an Asio *strand*; no global locks
  on the data plane.
* Server USB I/O: libusb asynchronous transfers; one dedicated libusb event thread
  (`libusb_handle_events_timeout_completed`). Completions are posted to the
  connection's strand.
* Client: VHCI fetch threads (blocking overlapped `DeviceIoControl` per thread)
  post into the connection strand; completions go back via COMPLETE IOCTL.
* Buffers: `BufferPool` with size classes (4 KiB … 1 MiB), lock-free free-list per
  class (Treiber stack with tagged index); falls back to heap for odd sizes.
* In-flight tracking: per-session `std::unordered_map<seq, Pending>` accessed only on
  the session strand.

### 2.6 Sessions and Smart USB Session Persistence

Every redirected device has a **Session** (128-bit random ID) with state machine:

```
Created → Authenticating → Attaching → Active ⇄ Suspended
                                   │      │  ⇅            │
                                   │      Reconnecting ───┘
                                   ▼      ▼
                                Failed  Detached → Closed
```
(the formal transition table lives in `Shared/Models/SessionState.*` and is unit tested).

* **Network loss**: Active → Suspended. Server keeps the device captured and the
  session reserved for `reconnect_timeout` (default 60 s). In-flight URBs are
  cancelled on the device and reported to the client as `Cancelled`. The client
  keeps its virtual device plugged (applications see a stalled, not removed, device)
  and reconnects with exponential backoff; `SESSION_RESUME` with the session
  resume token restores Active. On timeout the virtual device is unplugged.
* **Device re-enumeration (mode switch)**: the server's `DeviceMonitor` sees
  removal + arrival. `IdentityMatcher` scores the new device against devices that
  belonged to recently-ended sessions using: serial number (ignoring known
  placeholder serials), physical location path / port chain, parent hub, container
  ID, VID family, and time since removal. A score above threshold binds the new
  device to the existing session (state Reconnecting → Active), the server re-captures
  it (if `auto_capture` is on) and sends `DEVICE_REENUMERATED` with fresh
  descriptors; the client replugs the virtual device with the new descriptors so
  Windows loads the right driver for the new mode. VID/PID alone is never
  sufficient (two identical phones on one hub must not be confused).
* **ModeDetector** labels the mode (ADB, Fastboot, Qualcomm EDL 9008, MTK BROM,
  MTK Preloader, Samsung Download, …) from interface class triplets and a data-driven
  signature table (`Shared/Models/ModeSignatures.json`). Labels are informational
  and drive Mobile Repair Mode heuristics; they never gate redirection — any USB
  device can be shared.

### 2.7 Ownership, priority, access control

* A device has at most one **owner** session in **Exclusive** mode (default). Another
  client's attach is refused with `ERR_DEVICE_BUSY` (unless an admin forces
  detach via API).
* **Shared/read-only** mode: an additional observer can receive device status and
  descriptors but cannot submit URBs. Submitting URBs from multiple hosts to one
  device is unsafe in general, so v1 never multiplexes URB streams.
* **Priority** (Critical/High/Normal/Low) maps to the scheduling weight of a
  session's send queue and the libusb transfer submission order; Mobile Repair
  Mode defaults to High.
* ACL: per client identity (certificate fingerprint + token id) → allowed device
  filters (VID/PID/port globs), max sessions, admin flag.

### 2.8 Security

* TLS 1.3 only when `tls_enabled` (OpenSSL 3; Windows ships the OpenSSL DLLs with the
  app). Server identity = self-signed ECDSA P-256 certificate generated at first
  start; clients pin it (trust-on-first-use with fingerprint confirmation in the UI,
  or pre-provisioned fingerprint). Optional client certificates (mutual TLS).
* Authentication: API token (`pf_<id>_<secret>`). Server stores only
  PBKDF2-HMAC-SHA256 (210 000 iterations, 16-byte salt) hashes. Client stores its token
  encrypted with DPAPI (Windows) or 0600 file (other OSes).
* Session resume token: 32 random bytes, compared in constant time; invalidated on
  Closed.
* With TLS enabled the server refuses any non-TLS connection; there is no plaintext
  fallback. Plaintext mode exists for benchmarking on isolated LANs and is logged as a
  warning at every start.
* REST API: bound to 127.0.0.1 by default, bearer token, same hash store.
* Licensing is checked only at admission (connect / attach); never in the data path.

Details: [SECURITY.md](SECURITY.md).

### 2.9 Diagnostics and logging

* `Metrics`: lock-free counters and log-linear latency histograms (P50/P95/P99),
  per session and global: USB latency (submit→complete as seen by the server),
  network RTT (PING/PONG), upload/download throughput, USB throughput, timeouts,
  errors, resets, reconnects, packet loss (PING loss), process CPU and RAM.
* `Logger`: structured events (timestamp, level, component, client, device,
  session, operation, duration, result, error code, message), asynchronous writer,
  JSON-lines files with rotation, in-memory ring for the UI, `pflog` CLI and UI
  search (level/component/session/text filters).

### 2.10 Processes and UI

| Binary | Purpose |
|---|---|
| `ParsFlashServer.exe` | Qt 6 Widgets GUI + tray, hosts the server engine in-process |
| `parsflash-server.exe` | Headless server (console / Windows service) |
| `ParsFlashClient.exe` | Qt 6 Widgets GUI + tray, hosts the client engine in-process |
| `parsflash-client.exe` | Headless client CLI |
| `pfvhci.sys` + `pfvhci.inf` | Virtual host controller driver (client only) |
| `pfdevcon.exe` | Installs/removes the root-enumerated VHCI device (used by installer) |
| `pfbench.exe` | Network/session/USB benchmarks |
| `pflog.exe` | Log search |

Qt 6 Widgets was chosen over WinUI 3 because it gives first-class RTL layout mirroring
(`Qt::RightToLeft`), a mature translation system (Persian/English), system tray
support, MSVC/x64 builds, and lets the GUI be compiled/tested in CI. All
performance-critical code is in Qt-free native C++ libraries (`pf_shared`,
`pf_server_core`, `pf_client_core`); the GUI only observes engine state.

### 2.11 Source layout

```
Shared/   Common (config, buffers, errors, platform), Protocol, Models, Crypto,
          Logging, Diagnostics, Licensing, Network (transport)
Server/   Core, USB, Sessions, API, UI, App
Client/   Core, VirtualUSB, Sessions, UI, App
Driver/   Windows/KMDF (pfvhci), Windows/pfdevcon
Tests/    Unit, Integration, Network, USB, Stress
Tools/    pfbench, pflog
Installer/ Inno Setup scripts
Docs/     all documentation
```

### 2.12 Failure handling summary

| Failure | Handling |
|---|---|
| Server crash | Client: TCP reset → session Suspended → reconnect loop; virtual device kept for `reconnect_timeout`, then unplugged cleanly. Server restart loses session state (not persisted), client re-attaches by device identity. |
| Client crash | Driver unplugs devices of the dead file object; server sees disconnect → Suspended → Closed after timeout, device released. |
| USB unplug on server | DEVICE_REMOVED to owner; session → Reconnecting for `reconnect_timeout` awaiting re-enumeration match; pending URBs fail with `DeviceGone`. |
| Network loss | See 2.6. |
| Driver error | Client marks session Failed, logs NTSTATUS, unplugs device; other sessions unaffected. |
| USB reset failure | Reported as `USB_RESET` result error; session stays Active; client may retry or request re-enumeration. |
| Malformed frame | Connection closed with ERROR(PROTOCOL); never trusted lengths (bounded by `max_frame_size`). |
