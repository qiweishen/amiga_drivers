#ifndef LMS4XXX_ERROR_H
#define LMS4XXX_ERROR_H

#include <string>
#include <system_error>
#include <type_traits>


namespace LMS4xxx {

	// Error codes specific to the LMS4xxx driver.
	enum class ErrorCode : int {
		kSuccess = 0,

		// Connection errors
		kConnectionFailed = 100,   ///< TCP connection could not be established
		kConnectionTimeout = 101,  ///< Connection attempt timed out
		kConnectionLost = 102,	   ///< Unexpected disconnection during operation

		// Protocol errors
		kCrcError = 200,			///< Frame CRC8 checksum mismatch
		kProtocolError = 201,		///< Invalid CoLa B response format
		kUnexpectedResponse = 202,	///< Response does not match expected command
		kFrameTooShort = 203,		///< Received frame shorter than minimum length
		kFrameTooLong = 204,		///< Frame exceeds maximum expected length
		kResponseTimeout = 205,		///< Timed out waiting for device response

		// Device errors
		kDeviceError = 300,		 ///< Device reports error status
		kAccessDenied = 301,	 ///< Insufficient access level (login required)
		kCommandRejected = 302,	 ///< Device rejected the command

		// Configuration errors
		kInvalidConfig = 400,		///< Invalid configuration parameter
		kConfigFileNotFound = 401,	///< Configuration file does not exist
		kConfigParseError = 402,	///< JSON/YAML parse failure

		// Runtime errors
		kBufferOverflow = 500,	  ///< Ring buffer full, frame dropped
		kNotConnected = 501,	  ///< Operation requires an active connection
		kAlreadyConnected = 502,  ///< Connect called on active connection
		kAlreadyScanning = 503,	  ///< start_scanning called while already scanning
		kNotScanning = 504,		  ///< stop_scanning called while not scanning
		kThreadError = 505,		  ///< Failed to create or configure thread
	};


	// std::error_category implementation for LMS4xxx errors.
	class LMS4xxxErrorCategory : public std::error_category {
	public:
		[[nodiscard]] const char *name() const noexcept override { return "lms4xxx"; }

		[[nodiscard]] std::string message(int ev) const override {
			switch (static_cast<ErrorCode>(ev)) {
				case ErrorCode::kSuccess:
					return "success";
				case ErrorCode::kConnectionFailed:
					return "TCP connection failed";
				case ErrorCode::kConnectionTimeout:
					return "connection timed out";
				case ErrorCode::kConnectionLost:
					return "connection lost unexpectedly";
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
				case ErrorCode::kResponseTimeout:
					return "response timeout";
				case ErrorCode::kDeviceError:
					return "device reports error status";
				case ErrorCode::kAccessDenied:
					return "access denied (login required)";
				case ErrorCode::kCommandRejected:
					return "command rejected by device";
				case ErrorCode::kInvalidConfig:
					return "invalid configuration parameter";
				case ErrorCode::kConfigFileNotFound:
					return "configuration file not found";
				case ErrorCode::kConfigParseError:
					return "configuration parse error";
				case ErrorCode::kBufferOverflow:
					return "ring buffer overflow (frame dropped)";
				case ErrorCode::kNotConnected:
					return "not connected";
				case ErrorCode::kAlreadyConnected:
					return "already connected";
				case ErrorCode::kAlreadyScanning:
					return "already scanning";
				case ErrorCode::kNotScanning:
					return "not scanning";
				case ErrorCode::kThreadError:
					return "thread creation/config failed";
				default:
					return "unknown LMS4xxx error";
			}
		}

		// Singleton accessor.
		static const LMS4xxxErrorCategory &Instance() {
			static const LMS4xxxErrorCategory category;
			return category;
		}
	};


	// Factory function: create std::error_code from LMS4xxx::ErrorCode.
	inline std::error_code make_error_code(ErrorCode e) {
		return { static_cast<int>(e), LMS4xxxErrorCategory::Instance() };
	}

}  // namespace LMS4xxx


// Register LMS4xxx::ErrorCode as a valid error_code enum.
template<>
struct std::is_error_code_enum<LMS4xxx::ErrorCode> : std::true_type {};

#endif	// LMS4XXX_ERROR_H
