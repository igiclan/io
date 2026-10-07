#include "Common/Error.hpp"

namespace pf {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::NotSupported: return "NotSupported";
        case ErrorCode::NotImplemented: return "NotImplemented";
        case ErrorCode::ResourceExhausted: return "ResourceExhausted";
        case ErrorCode::Internal: return "Internal";
        case ErrorCode::Io: return "Io";
        case ErrorCode::Busy: return "Busy";
        case ErrorCode::InvalidState: return "InvalidState";
        case ErrorCode::ProtocolViolation: return "ProtocolViolation";
        case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
        case ErrorCode::FrameTooLarge: return "FrameTooLarge";
        case ErrorCode::BadMagic: return "BadMagic";
        case ErrorCode::UnexpectedMessage: return "UnexpectedMessage";
        case ErrorCode::MalformedPayload: return "MalformedPayload";
        case ErrorCode::AuthRequired: return "AuthRequired";
        case ErrorCode::AuthFailed: return "AuthFailed";
        case ErrorCode::AccessDenied: return "AccessDenied";
        case ErrorCode::TlsRequired: return "TlsRequired";
        case ErrorCode::TlsFailure: return "TlsFailure";
        case ErrorCode::LicenseLimit: return "LicenseLimit";
        case ErrorCode::SessionExpired: return "SessionExpired";
        case ErrorCode::ConnectionLost: return "ConnectionLost";
        case ErrorCode::ConnectionRefused: return "ConnectionRefused";
        case ErrorCode::HostUnreachable: return "HostUnreachable";
        case ErrorCode::DeviceNotFound: return "DeviceNotFound";
        case ErrorCode::DeviceBusy: return "DeviceBusy";
        case ErrorCode::DeviceGone: return "DeviceGone";
        case ErrorCode::DeviceAccess: return "DeviceAccess";
        case ErrorCode::DeviceResetFailed: return "DeviceResetFailed";
        case ErrorCode::TooManyDevices: return "TooManyDevices";
        case ErrorCode::TooManyClients: return "TooManyClients";
        case ErrorCode::DriverNotInstalled: return "DriverNotInstalled";
        case ErrorCode::DriverVersionMismatch: return "DriverVersionMismatch";
        case ErrorCode::DriverError: return "DriverError";
    }
    return "Unrecognized";
}

std::string Error::describe() const {
    std::string s{to_string(code)};
    if (!message.empty()) {
        s += ": ";
        s += message;
    }
    return s;
}

}  // namespace pf
