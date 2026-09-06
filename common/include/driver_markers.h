#pragma once

#include <string_view>


namespace common::Markers {
    // Module tokens: the "[Module]:" tag of every log line
    // The GUI routes a line to a sensor's health state machine by exact module comparison
    inline constexpr std::string_view kModuleMain = "MainApp";
    inline constexpr std::string_view kModuleAsterx = "AsteRxApp";
    inline constexpr std::string_view kModuleFx10 = "FX10App";
    inline constexpr std::string_view kModuleGox = "GoXApp";
    inline constexpr std::string_view kModuleLms4xxx = "LMS4xxxApp";

    // Driver lifecycle markers (verbatim, matched by substring)
    inline constexpr std::string_view kAsterxInitialized = "AsteRx driver initialized";
    inline constexpr std::string_view kFx10Initialized = "FX10 driver initialized";
    inline constexpr std::string_view kGoxInitialized = "GoX driver initialized";
    inline constexpr std::string_view kLmsInitialized = "LMS4xxx driver initialized";

    inline constexpr std::string_view kAsterxShutdown = "AsteRx driver shutdown completely";
    inline constexpr std::string_view kFx10Shutdown = "FX10 driver shutdown completely";
    inline constexpr std::string_view kGoxShutdown = "GoX driver shutdown completely";
    inline constexpr std::string_view kLmsShutdown = "LMS4xxx driver shutdown completely";

    inline constexpr std::string_view kAsterxSessionIssues = "AsteRx driver ended with issues";
    inline constexpr std::string_view kFx10SessionIssues = "FX10 driver ended with issues";
    inline constexpr std::string_view kGoxSessionIssues = "GoX driver ended with issues";

    // fmt templates ({} = GoX / LiDAR instance name); format via fmt::runtime(...)
    inline constexpr std::string_view kGoxInitializedInstTpl = "GoX instance [{}] initialized successfully";
    inline constexpr std::string_view kGoxShutdownInstTpl = "GoX instance [{}] driver shutdown completely";
    inline constexpr std::string_view kLmsInitializedInstTpl = "LiDAR instance [{}] initialized successfully";
    inline constexpr std::string_view kLmsShutdownInstTpl = "LiDAR instance [{}] driver shutdown completely";

    // Unified-main markers. kAllDriversShutDown is the GUI's ONLY evidence of a clean exit
    inline constexpr std::string_view kStartingDrivers = "Starting Amiga Drivers";
    inline constexpr std::string_view kReceivedSignalTpl = "Received signal {}, shutting down all drivers...";
    inline constexpr std::string_view kAllDriversShutDown = "All drivers shut down";

    // Per-driver failure markers emitted by Main
    // The GUI maps the message to a  sensor by its leading driver name (AsteRx/FX10/GoX/LMS4xxx)
    inline constexpr std::string_view kAsterxInitFailed = "AsteRx driver initialization failed";
    inline constexpr std::string_view kFx10InitFailed = "FX10 driver initialization failed";
    inline constexpr std::string_view kGoxInitFailed = "GoX driver initialization failed";
    inline constexpr std::string_view kLms4xxxInitFailed = "LMS4xxx driver initialization failed";
    // Appended to a driver name by Main's no-data watchdog, e.g.
    // "FX10 driver stopped: no data (silent for 31.0 s, ...)". Same
    // "<DriverName><suffix>" shape as the two markers above, so the GUI routes
    // it to that sensor's card. The disk guard has no such marker on purpose:
    // it is not any one sensor's fault, and the GUI already shows free space.
    inline constexpr std::string_view kGuardNoDataSuffix = " driver stopped: no data";

    inline constexpr std::string_view kAsterxRunException = "AsteRx run() exception";
    inline constexpr std::string_view kFx10RunException = "FX10 run() exception";
    inline constexpr std::string_view kGoxRunException = "GoX run() exception";
    inline constexpr std::string_view kLms4xxxRunException = "LMS4xxx run() exception";
} // namespace common::Markers
