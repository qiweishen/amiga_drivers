#pragma once

#include <string>
#include <system_error>
#include <type_traits>


namespace lms4xxx {
    // Driver-level failures; connection-level codes are TcpError (lms4xxx_tcp_client.h)
    enum class ErrorCode : int {
        // Protocol errors
        kCrcError = 200, ///< Frame CRC8 checksum mismatch
        kProtocolError = 201, ///< Invalid CoLa B response format
        kUnexpectedResponse = 202, ///< Response does not match expected command
        kFrameTooShort = 203, ///< Received frame shorter than minimum length
        kFrameTooLong = 204, ///< Frame exceeds maximum expected length

        // Device errors
        kAccessDenied = 301, ///< Insufficient access level (login required)
        kCommandRejected = 302, ///< Device rejected the command
        kSopasError = 303, ///< Device answered sFA (SOPAS error code)
        kReadbackMismatch = 304, ///< Readback differs from the written value
        kDeviceNotReady = 305, ///< SCdevicestate did not reach Ready

        // Configuration errors
        kInvalidConfig = 400, ///< Invalid configuration parameter

        // Runtime errors
        kNotConnected = 501, ///< Operation requires an active connection
        kAlreadyScanning = 503, ///< StartScanning called while already scanning
        kNotScanning = 504, ///< StopScanning called while not scanning
    };


    class Lms4xxxErrorCategory : public std::error_category {
    public:
        [[nodiscard]] const char *name() const noexcept override { return "lms4xxx"; }

        [[nodiscard]] std::string message(int ev) const override {
            switch (static_cast<ErrorCode>(ev)) {
                case ErrorCode::kCrcError:
                    return "CRC8 checksum mismatch";
                case ErrorCode::kProtocolError:
                    return "invalid CoLa B response";
                case ErrorCode::kUnexpectedResponse:
                    return "unexpected response type";
                case ErrorCode::kFrameTooShort:
                    return "frame too short";
                case ErrorCode::kFrameTooLong:
                    return "frame exceeds max length";
                case ErrorCode::kAccessDenied:
                    return "access denied (login required)";
                case ErrorCode::kCommandRejected:
                    return "command rejected by device";
                case ErrorCode::kSopasError:
                    return "device answered sFA";
                case ErrorCode::kReadbackMismatch:
                    return "device stores a different value than written";
                case ErrorCode::kDeviceNotReady:
                    return "device did not reach the ready state";
                case ErrorCode::kInvalidConfig:
                    return "invalid configuration parameter";
                case ErrorCode::kNotConnected:
                    return "not connected";
                case ErrorCode::kAlreadyScanning:
                    return "already scanning";
                case ErrorCode::kNotScanning:
                    return "not scanning";
                default:
                    return "unknown LMS4xxx error";
            }
        }

        static const Lms4xxxErrorCategory &Instance() {
            static const Lms4xxxErrorCategory category;
            return category;
        }
    };


    inline std::error_code make_error_code(ErrorCode e) {
        return {static_cast<int>(e), Lms4xxxErrorCategory::Instance()};
    }
} // namespace lms4xxx


template<>
struct std::is_error_code_enum<lms4xxx::ErrorCode> : std::true_type {
};
