/// @file driver_markers.h
/// @brief GUI contract — single source of truth for the lifecycle marker
/// strings the web GUI parses out of the unified log.
///
/// The Python mirror is app/services/markers.py; the two files must agree
/// VERBATIM. Run `uv run python tools/check_contracts.py` after editing either
/// side. Do NOT reword any value here without updating the mirror — the GUI
/// health state machine (app/services/health.py) and the reattach clean-exit
/// detection (app/services/process.py) match these strings literally.

#ifndef COMMON_DRIVER_MARKERS_H
#define COMMON_DRIVER_MARKERS_H

#include <string_view>


namespace Common::Markers {
	// Module tokens: the "[Module]:" tag of every log line
	// The GUI routes a line to a sensor's health state machine by exact module comparison
	inline constexpr std::string_view kModuleMain = "MainApp";
	inline constexpr std::string_view kModuleAsterx = "AsterxApp";
	inline constexpr std::string_view kModuleFx10 = "FX10App";
	inline constexpr std::string_view kModuleGox = "GoXApp";
	inline constexpr std::string_view kModuleIns401 = "INS401App";
	inline constexpr std::string_view kModuleLms4xxx = "LMS4xxxApp";

	// Driver lifecycle markers (verbatim, matched by substring)
	inline constexpr std::string_view kAsterxInitialized = "AsteRx driver initialized";
	inline constexpr std::string_view kAsterxShutdown = "AsteRx driver shutdown completely";
	inline constexpr std::string_view kFx10Initialized = "FX10 driver initialized";
	inline constexpr std::string_view kFx10Shutdown = "FX10 driver shutdown completely";
	// Prefix only; the full line appends " (standalone exit code N)"
	inline constexpr std::string_view kFx10SessionIssues = "FX10 driver ended with issues";
	inline constexpr std::string_view kGoxInitialized = "GoX driver initialized";
	inline constexpr std::string_view kGoxShutdown = "GoX driver shutdown completely";
	// Prefix only; the full line appends " (<concrete reason>)"
	inline constexpr std::string_view kGoxSessionIssues = "GoX driver ended with issues";
	inline constexpr std::string_view kIns401Initialized = "INS401 driver initialized";
	inline constexpr std::string_view kIns401Shutdown = "INS401 driver shutdown completely";

	// fmt templates ({} = LiDAR instance name); format via fmt::runtime(...)
	inline constexpr std::string_view kLmsInitializedTpl = "LiDAR instance [{}] initialized successfully";
	inline constexpr std::string_view kLmsShutdownTpl = "LiDAR instance [{}] driver shutdown completely";

	// Unified-main markers. kAllDriversShutDown is the GUI's ONLY evidence of a
	// clean exit (reattach after a GUI restart depends on it).
	inline constexpr std::string_view kStartingDrivers = "Starting Amiga Drivers";
	inline constexpr std::string_view kReceivedSignalTpl = "Received signal {}, shutting down all drivers...";
	inline constexpr std::string_view kAllDriversShutDown = "All drivers shut down";

	// Per-driver failure markers emitted by Main
	// The GUI maps the message to a  sensor by its leading driver name (AsteRx/FX10/GoX/INS401/LMS4xxx)
	inline constexpr std::string_view kAsterxInitFailed = "AsteRx driver initialization failed";
	inline constexpr std::string_view kFx10InitFailed = "FX10 driver initialization failed";
	inline constexpr std::string_view kGoxInitFailed = "GoX driver initialization failed";
	inline constexpr std::string_view kIns401InitFailed = "INS401 driver initialization failed";
	inline constexpr std::string_view kLms4xxxInitFailed = "LMS4xxx driver initialization failed";
	inline constexpr std::string_view kAsterxRunException = "AsteRx run() exception";
	inline constexpr std::string_view kFx10RunException = "FX10 run() exception";
	inline constexpr std::string_view kGoxRunException = "GoX run() exception";
	inline constexpr std::string_view kIns401RunException = "INS401 run() exception";
	inline constexpr std::string_view kLms4xxxRunException = "LMS4xxx run() exception";
} // namespace Common::Markers

#endif // COMMON_DRIVER_MARKERS_H
