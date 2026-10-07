# PFUP/1 — ParsFlash USB Protocol, version 1

Normative specification of the wire protocol between ParsFlash USB Server and
ParsFlash USB Client. Reference implementation: `Shared/Protocol/Protocol.{hpp,cpp}`
(unit tests: `Tests/Unit/test_protocol.cpp`).

Key words MUST, SHOULD, MAY are used as in RFC 2119.

## 1. Transport

* A PFUP connection is a reliable, ordered byte stream. Version 1 defines TCP
  (default port **39000**, configurable) optionally wrapped in **TLS 1.3**.
* When the server runs with `tls_enabled = true`, the TLS handshake MUST complete
  before the first PFUP frame; a server in TLS mode never accepts plaintext frames.
  TLS 1.2 and older are refused.
* Both sides set `TCP_NODELAY`. Transport keepalive (SO_KEEPALIVE) is enabled; the
  application-level PING (§6) is authoritative for liveness.
* Future transports (QUIC, named pipe) reuse the framing unchanged.

## 2. Framing

Every message is a frame: a fixed 16-byte header followed by `payload_size` bytes.
All integers are **little-endian**.

| Offset | Size | Field | Description |
|---:|---:|---|---|
| 0 | 2 | `magic` | `0x4650` (bytes `'P' 'F'`) |
| 2 | 1 | `version` | protocol version of this frame, `1` |
| 3 | 1 | `type` | message type (§3) |
| 4 | 2 | `flags` | bit0 `JSON` payload is UTF-8 JSON object; bit1 `RESPONSE`; others 0 |
| 6 | 2 | `header_size` | `16` (reserved for header extensions) |
| 8 | 4 | `payload_size` | bytes following the header |
| 12 | 4 | `correlation` | control plane request/response id; 0 when unused |

Receivers MUST close the connection (after an optional `ERROR`) when: magic is
wrong, version is outside the negotiated range, `header_size != 16`, the type is
unknown, or `payload_size` exceeds `max_frame_size` (default 16 MiB + 64 KiB,
configurable). Receivers MUST NOT allocate based on `payload_size` before this
check.

## 3. Message types

| Code | Name | Dir | Payload | Plane |
|---:|---|---|---|---|
| 0x01 | HELLO | C→S | JSON | control |
| 0x02 | HELLO_ACK | S→C | JSON | control |
| 0x03 | AUTH | C→S | JSON | control |
| 0x04 | AUTH_RESULT | S→C | JSON | control |
| 0x05 | SESSION_RESUME | C→S | JSON | control |
| 0x06 | SESSION_RESUME_RESULT | S→C | JSON | control |
| 0x10 | DEVICE_LIST_REQUEST | C→S | JSON | control |
| 0x11 | DEVICE_LIST | S→C | JSON | control |
| 0x12 | DEVICE_EVENT | S→C | JSON | control |
| 0x20 | DEVICE_ATTACH | C→S | JSON | control |
| 0x21 | DEVICE_ATTACH_RESULT | S→C | JSON | control |
| 0x22 | DEVICE_DETACH | C→S | JSON | control |
| 0x23 | DEVICE_DETACH_RESULT | S→C | JSON | control |
| 0x24 | DEVICE_REENUMERATED | S→C | JSON | control |
| 0x30 | USB_SUBMIT (USB_REQUEST) | C→S | binary | data |
| 0x31 | USB_COMPLETE (USB_RESPONSE) | S→C | binary | data |
| 0x32 | USB_CANCEL | C→S | binary | data |
| 0x33 | USB_RESET | C→S | binary | data |
| 0x34 | USB_OP_RESULT | S→C | binary | data |
| 0x35 | USB_CONFIG | C→S | binary | data |
| 0x40 | PING | both | binary | link |
| 0x41 | PONG | both | binary | link |
| 0x7E | GOODBYE | both | JSON | link |
| 0x7F | ERROR | both | JSON | link |

Codes are never reused. New types require a new protocol version or a capability
(§4) that both peers advertised.

## 4. Connection establishment and versioning

```
C: [TLS handshake]
C: HELLO        {"protocol":[1], "client_id":"…", "client_name":"PC-01",
                 "app_version":"1.0.0", "capabilities":["iso","resume","batch"]}
S: HELLO_ACK    {"protocol":1, "server_id":"…", "server_name":"BENCH-01",
                 "app_version":"1.0.0", "auth_required":true,
                 "capabilities":[…], "max_frame_size":16842752,
                 "keepalive_ms":5000, "reconnect_timeout_ms":60000}
C: AUTH         {"method":"token", "token":"pf_<id>_<secret>"}
S: AUTH_RESULT  {"ok":true, "client_id":"…", "admin":false}
```

* `protocol` in HELLO lists every version the client supports; the server picks
  the highest common version or answers `ERROR{UnsupportedVersion}` and closes.
  Every subsequent frame carries the negotiated version.
* `capabilities` are free-form strings; only the intersection is used.
* Until AUTH succeeds the server accepts only HELLO, AUTH, PING, GOODBYE.
  Any other frame → `ERROR{AuthRequired}` and close. After 3 failed AUTH attempts or
  `auth_timeout` (10 s) the connection is closed.
* `client_id` is a stable random UUID per client installation (used for session
  ownership and ACLs together with the token identity).

## 5. Control plane (JSON)

Control frames set the `JSON` flag. Requests carry a non-zero `correlation`; the
response echoes it and sets `RESPONSE`. Unknown JSON fields MUST be ignored.

### 5.1 Device descriptor object

```json
{
  "id": "d3f2a9c01b77e",            // server-assigned, stable per physical port
  "vid": 6353, "pid": 20199,
  "serial": "28131FDH2000AB",
  "manufacturer": "Google", "product": "Pixel 7",
  "bcd_usb": 528, "bcd_device": 1088,
  "class": 0, "subclass": 0, "protocol": 0,
  "speed": "high",
  "port_path": "1-4.2", "location": "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(4)#USB(2)",
  "parent": "USB\\VID_05E3&PID_0610\\…", "container_id": "{…}",
  "mode": "adb", "mode_label": "ADB", "mode_family": "android", "repair_mode": true,
  "state": "available",              // available | shared | in-use | unavailable
  "shared": true, "owner": "client-id-or-empty", "session_id": "",
  "access_error": ""                 // why the server cannot capture it (if any)
}
```

### 5.2 Messages

| Message | Body |
|---|---|
| DEVICE_LIST_REQUEST | `{}` |
| DEVICE_LIST | `{"devices":[<device>…]}` — only devices this client may see (ACL) |
| DEVICE_EVENT | `{"event":"arrived"|"removed"|"changed", "device":<device>}` |
| DEVICE_ATTACH | `{"device_id":"…", "access":"exclusive"|"shared-readonly", "priority":"low"|"normal"|"high"|"critical", "mobile_repair":true}` |
| DEVICE_ATTACH_RESULT | `{"ok":true, "handle":7, "session_id":"<32 hex>", "resume_token":"<64 hex>", "speed":"high", "descriptors":<descriptor set>, "device":<device>}` or `{"ok":false,"code":401,"message":"…"}` |
| DEVICE_DETACH | `{"handle":7}` |
| DEVICE_DETACH_RESULT | `{"ok":true}` |
| DEVICE_REENUMERATED | `{"session_id":"…", "handle":7, "descriptors":<descriptor set>, "device":<device>, "speed":"high"}` |
| SESSION_RESUME | `{"session_id":"…", "resume_token":"…"}` |
| SESSION_RESUME_RESULT | same shape as DEVICE_ATTACH_RESULT |
| ERROR | `{"code":<ErrorCode>, "name":"DeviceBusy", "message":"…"}` |
| GOODBYE | `{"reason":"…"}` |

Descriptor set:

```json
{"device":"<base64 18 bytes>", "configs":["<base64 full config>"…],
 "bos":"<base64 or empty>", "qualifier":"<base64 or empty>",
 "strings":[{"index":0,"lang":0,"data":"<base64>"}, …]}
```

`handle` is a 32-bit server-assigned identifier used in every data-plane frame for
that attachment. It is unique per connection and is **kept across SESSION_RESUME
and DEVICE_REENUMERATED** so in-flight bookkeeping on the client survives.

Error codes are the `pf::ErrorCode` values (`Shared/Common/Error.hpp`), e.g. 200
AuthRequired, 201 AuthFailed, 202 AccessDenied, 205 LicenseLimit, 206 SessionExpired,
400 DeviceNotFound, 401 DeviceBusy, 402 DeviceGone, 403 DeviceAccess, 405
TooManyDevices, 406 TooManyClients.

## 6. Data plane (binary)

All data-plane frames have `flags = 0` (or `RESPONSE` for server→client results)
and `correlation = 0`; correlation uses the 64-bit `seq` inside the payload.
`seq` is chosen by the client, MUST be unique among the client's outstanding
requests on that connection, and SHOULD be monotonically increasing.

### 6.1 USB_SUBMIT (0x30)

| Off | Size | Field |
|---:|---:|---|
| 0 | 8 | `seq` |
| 8 | 4 | `handle` |
| 12 | 1 | `endpoint` (`bEndpointAddress`; EP0 = 0x00 or 0x80) |
| 13 | 1 | `type` 0 control, 1 isochronous, 2 bulk, 3 interrupt |
| 14 | 2 | `flags` bit0 SHORT_NOT_OK, bit1 ZERO_PACKET, bit2 DIR_IN |
| 16 | 4 | `timeout_ms` (0 = none) |
| 20 | 4 | `length` (IN: bytes requested; OUT: bytes of data that follow) |
| 24 | 8 | `setup` (control only; zero otherwise) |
| 32 | 4 | `iso_packet_count` |
| 36 | 4 | reserved (0) |
| 40 | 8·n | iso packets: `offset u32, length u32`, each within `length` |
| … | `length` | OUT data (OUT only) |

Validation: for control transfers the direction is `setup[0] & 0x80` and MUST
equal `DIR_IN`; for others the endpoint bit 7. IN submits carry no data. Iso
packets are only allowed on isochronous transfers.

### 6.2 USB_COMPLETE (0x31)

| Off | Size | Field |
|---:|---:|---|
| 0 | 8 | `seq` |
| 8 | 4 | `handle` |
| 12 | 4 | `status` (`pf::UsbStatus`: 0 success, 1 stall, 2 timeout, 3 cancelled, 4 overflow, 5 short-packet, 6 device-gone, 7 no-device, 8 invalid-request, 9 not-supported, 10 io-error, 11 busy, 12 no-resources, 13 not-authorized) |
| 16 | 4 | `actual_length` |
| 20 | 4 | `iso_packet_count` |
| 24 | 4 | `iso_error_count` |
| 28 | 4 | reserved |
| 32 | 8·n | iso results: `actual_length u32, status i32` |
| … | … | IN data |

For non-iso IN transfers the data size MUST equal `actual_length`; OUT completions
carry no data. For iso IN the data is the transfer buffer from offset 0 to the end of
the last packet (packet `offset`s from the submit locate each packet).

Every USB_SUBMIT receives exactly one USB_COMPLETE (also after USB_CANCEL — then
normally with status `cancelled`, or the real result if it finished first).

### 6.3 USB_CANCEL (0x32)

`seq u64, handle u32`. Best effort; the server cancels the in-flight transfer. No
direct reply (the USB_COMPLETE of the original seq is the reply).

### 6.4 USB_RESET (0x33)

`seq u64, handle u32, kind u8 (0 port reset, 1 re-enumerate), pad[3]`. Reply:
USB_OP_RESULT. After a re-enumerate the server sends DEVICE_REENUMERATED.

### 6.5 USB_CONFIG (0x35)

`seq u64, handle u32, op u8 (0 SetConfiguration, 1 SetInterface, 2 ClearHalt),
value u8 (configuration value | interface number | endpoint), alt u8, pad u8`.
Reply: USB_OP_RESULT.

### 6.6 USB_OP_RESULT (0x34)

`seq u64, handle u32, status i32`.

### 6.7 PING / PONG (0x40/0x41)

`seq u64, sent_ns u64`. PONG echoes both fields. Either side MAY ping; default
interval `keepalive_ms` = 5000 when idle (only sent if nothing was received during
the interval, so idle traffic is ~32 bytes/5 s). Missing `keepalive_miss_limit` (3)
consecutive PONGs → connection considered lost. RTT = now − sent_ns on the
originator's monotonic clock.

## 7. Sessions and resume

* DEVICE_ATTACH creates a session (state Active on success) owned by the
  authenticated client identity. `resume_token` is a 256-bit secret known only to
  the two peers.
* Connection loss: the server moves the session to **Suspended**, cancels in-flight
  transfers and keeps the device captured for `reconnect_timeout` (default 60 s).
* The client reconnects (HELLO, AUTH) and sends SESSION_RESUME for each session.
  The server verifies the token in constant time, checks the session belongs to the
  same client identity, and answers SESSION_RESUME_RESULT with fresh descriptors (the
  device may have changed mode meanwhile). Session state → Active.
* Transfers that were in flight at disconnect are **not replayed** (USB requests are
  not idempotent). The client completes them toward Windows with a device-level
  error so the application retries at its own protocol layer.
* Device re-enumeration (mode switch) while Active: session → Reconnecting; when
  the matcher binds a new device, the server sends DEVICE_REENUMERATED and the
  session returns to Active; if nothing matches within `reconnect_timeout` the server
  sends `DEVICE_EVENT{removed}` and closes the session.

## 8. Flow control and ordering

* TCP provides ordering. The server executes submits for the **same endpoint in
  order**; submits for different endpoints are independent.
* Each peer bounds unsent bytes per connection (`send_queue_high_watermark`, default
  32 MiB). When exceeded, the client stops fetching new URBs from the driver and the
  server stops reading from the socket until below the low watermark — TCP back
  pressure then throttles the peer. Nothing is dropped.
* Senders MAY coalesce several frames into one write; receivers MUST NOT assume one
  frame per segment.

## 9. Security considerations

* TLS 1.3 protects confidentiality/integrity; server identity is the SHA-256
  fingerprint of its certificate, pinned by the client.
* Tokens are only sent inside TLS; in plaintext mode (benchmark/isolated LAN only)
  the server logs a warning and the client UI shows "Not encrypted".
* All lengths are validated before use (§2, §6); malformed frames close the
  connection. Fuzz tests: `Tests/Unit/test_protocol.cpp`.

## 10. Version history

| Version | Changes |
|---|---|
| 1 | Initial version |
