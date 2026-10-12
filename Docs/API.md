# ParsFlash USB Server — Management REST API

Status: **implemented and tested** for the endpoints marked ✅. `attach` is intentionally
not available yet (see below).

## Transport and security

- HTTP/1.1, one request per connection (`Connection: close`), JSON request and response
  bodies, `Cache-Control: no-store`.
- Bound to `127.0.0.1:39001` by default (`api.bind_address`, `api.port` in `config.json`).
  The API is **plaintext**: bearer tokens are not encrypted on the wire. Use it locally or
  through an SSH tunnel or VPN. The server logs a warning when bound to any other address.
- Authentication: `Authorization: Bearer pf_<id>_<secret>`. Tokens are created with the
  TokenStore (only salted keyed hashes are stored).
  - Any valid token may **read**.
  - Only tokens created with the admin flag may **change state** (403 otherwise).
- Limits: request head ≤ 16 KiB, body ≤ 64 KiB, idle connection timeout 10 s.
  `Transfer-Encoding` (chunked) is refused; duplicate `Content-Length` is refused.
- No CORS headers are sent, so browsers on other origins cannot call the API.

## Errors

```json
{"error": {"code": "DeviceBusy", "message": "device in use by PC-01"}}
```

| HTTP | Meaning |
|---|---|
| 400 | malformed request, body or JSON |
| 401 | missing or invalid token |
| 403 | valid token without the admin flag |
| 404 | unknown route or device |
| 405 | method not allowed on this route |
| 409 | state conflict (e.g. detach of a device that is not attached) |
| 413 | request too large |
| 501 | not implemented (attach) |

## Endpoints

| Method | Path | Role | Result |
|---|---|---|---|
| GET | `/api/status` ✅ | read | server status, counters, license, CPU/RAM |
| GET | `/api/devices` ✅ | read | array of devices (VID/PID, serial, speed, port, mode, state, owner) |
| GET | `/api/devices/{id}` ✅ | read | one device, 404 if absent |
| GET | `/api/clients` ✅ | read | connected, authenticated clients with RTT and throughput |
| GET | `/api/sessions` ✅ | read | live sessions with state, owner, device, in-flight count and USB metrics |
| GET | `/api/metrics` ✅ | read | per-session and per-link metrics, buffer pool, logger drop count |
| POST | `/api/devices/{id}/share` ✅ | admin | body `{"shared": true|false}` (default `true`); stopping sharing closes the active session |
| POST | `/api/devices/{id}/reset` ✅ | admin | USB port reset; returns 202 (completes asynchronously) |
| POST | `/api/devices/{id}/detach` ✅ | admin | closes the active session; device returns to shared/available |
| POST | `/api/devices/{id}/attach` | admin | **501**: attach is started by a ParsFlash USB Client, which owns the data path |

Device `state` values: `available`, `shared`, `in-use`, `unavailable`.

## Example

```sh
curl -s -H "Authorization: Bearer $PF_TOKEN" http://127.0.0.1:39001/api/devices
curl -s -X POST -H "Authorization: Bearer $PF_TOKEN" \
     -d '{"shared": false}' http://127.0.0.1:39001/api/devices/d1a2b3c4d5e6f/share
```

Tests: `Tests/Integration/test_rest_api.cpp` (real HTTP over loopback, role checks,
limits, malformed input, framing unit checks).
