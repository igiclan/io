// ParsFlash USB Redirector - https://parsflash.com
// Error codes shared by every component (protocol, logs, API).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace pf {

// Numeric values are part of the wire protocol (ERROR message, AUTH_RESULT,
// DEVICE_ATTACH_RESULT) and of log records. Never renumber; only append.
enum class ErrorCode : std::int32_t {
    Ok = 0,
    // generic 1..99
    Unknown = 1,
    InvalidArgument = 2,
    NotFound = 3,
    AlreadyExists = 4,
    Timeout = 5,
    Cancelled = 6,
    NotSupported = 7,
    NotImplemented = 8,
    ResourceExhausted = 9,
    Internal = 10,
    Io = 11,
    Busy = 12,
    InvalidState = 13,
    // protocol 100..199
    ProtocolViolation = 100,
    UnsupportedVersion = 101,
    FrameTooLarge = 102,
    BadMagic = 103,
    UnexpectedMessage = 104,
    MalformedPayload = 105,
    // security 200..299
    AuthRequired = 200,
    AuthFailed = 201,
    AccessDenied = 202,
    TlsRequired = 203,
    TlsFailure = 204,
    LicenseLimit = 205,
    SessionExpired = 206,
    // network 300..399
    ConnectionLost = 300,
    ConnectionRefused = 301,
    HostUnreachable = 302,
    // usb/device 400..499
    DeviceNotFound = 400,
    DeviceBusy = 401,
    DeviceGone = 402,
    DeviceAccess = 403,   // OS refused access (driver not capturable, permissions)
    DeviceResetFailed = 404,
    TooManyDevices = 405,
    TooManyClients = 406,
    // driver 500..599
    DriverNotInstalled = 500,
    DriverVersionMismatch = 501,
    DriverError = 502,
};

std::string_view to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg = {}) : code(c), message(std::move(msg)) {}
    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
    explicit operator bool() const noexcept { return !ok(); }  // true when an error is set
    [[nodiscard]] std::string describe() const;
};

// Minimal expected-like type (std::expected is C++23).
template <typename T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}            // NOLINT(google-explicit-constructor)
    Result(Error err) : v_(std::move(err)) {}            // NOLINT(google-explicit-constructor)
    Result(ErrorCode c, std::string msg = {}) : v_(Error{c, std::move(msg)}) {}

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(v_); }
    explicit operator bool() const noexcept { return ok(); }
    T& value() & { return std::get<T>(v_); }
    const T& value() const& { return std::get<T>(v_); }
    T&& value() && { return std::get<T>(std::move(v_)); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    [[nodiscard]] const Error& error() const { return std::get<Error>(v_); }

private:
    std::variant<T, Error> v_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error err) : e_(std::move(err)) {}            // NOLINT(google-explicit-constructor)
    Result(ErrorCode c, std::string msg = {}) : e_(c, std::move(msg)) {}
    [[nodiscard]] bool ok() const noexcept { return e_.ok(); }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const { return e_; }

private:
    Error e_;
};

using Status = Result<void>;

}  // namespace pf
