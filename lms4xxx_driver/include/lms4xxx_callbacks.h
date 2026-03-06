#ifndef LMS4XXX_CALLBACKS_H
#define LMS4XXX_CALLBACKS_H

#include <cstdint>
#include <functional>
#include <string>
#include <system_error>


namespace LMS4xxx {

	// Forward declarations
	struct ScanData;


	// Connection state enumeration
	enum class ConnectionState : std::uint8_t {
		kDisconnected = 0,
		kConnecting = 1,
		kConnected = 2,
		kConfiguring = 3,
		kScanning = 4,
		kError = 5,
	};


	// Convert ConnectionState to human-readable string
	[[nodiscard]] inline const char *ToString(ConnectionState state) {
		switch (state) {
			case ConnectionState::kDisconnected:
				return "Disconnected";
			case ConnectionState::kConnecting:
				return "Connecting";
			case ConnectionState::kConnected:
				return "Connected";
			case ConnectionState::kConfiguring:
				return "Configuring";
			case ConnectionState::kScanning:
				return "Scanning";
			case ConnectionState::kError:
				return "Error";
			default:
				return "Unknown";
		}
	}


	// Callback invoked for each complete scan frame. Must not block or throw
	using ScanDataCallback = std::function<void(const ScanData &scan_data)>;

	// Callback invoked when the connection state changes
	using ConnectionStateCallback = std::function<void(ConnectionState new_state)>;

	// Callback invoked when an error occurs. Must be thread-safe
	using ErrorCallback = std::function<void(std::error_code ec, const std::string &detail)>;

} // namespace LMS4xxx

#endif	// LMS4XXX_CALLBACKS_H
