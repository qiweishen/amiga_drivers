#pragma once

#include <string_view>


namespace common::Markers {
    // Module tokens: the "[Module]:" tag of every log line
    // Diagnostics/statistics display only; lifecycle comes from amiga-run-v1.
    inline constexpr std::string_view kModuleMain = "MainApp";
    inline constexpr std::string_view kModuleAsterx = "AsteRxApp";
    inline constexpr std::string_view kModuleFx10 = "FX10App";
    inline constexpr std::string_view kModuleGox = "GoXApp";
    inline constexpr std::string_view kModuleLms4xxx = "LMS4xxxApp";

    // Human-readable lifecycle diagnostics (not a control protocol).
    inline constexpr std::string_view kAsterxInitialized = "AsteRx driver initialized";
    inline constexpr std::string_view kFx10Initialized = "FX10 driver initialized";
    inline constexpr std::string_view kGoxInitialized = "GoX driver initialized";

    inline constexpr std::string_view kAsterxShutdown = "AsteRx driver shutdown completely";
    inline constexpr std::string_view kFx10Shutdown = "FX10 driver shutdown completely";
    inline constexpr std::string_view kGoxShutdown = "GoX driver shutdown completely";

    inline constexpr std::string_view kAsterxSessionIssues = "AsteRx driver ended with issues";
    inline constexpr std::string_view kFx10SessionIssues = "FX10 driver ended with issues";
    inline constexpr std::string_view kGoxSessionIssues = "GoX driver ended with issues";

    // fmt templates ({} = GoX / LiDAR instance name); format via fmt::runtime(...)
    inline constexpr std::string_view kGoxInitializedInstTpl = "GoX instance [{}] initialized successfully";
    inline constexpr std::string_view kGoxShutdownInstTpl = "GoX instance [{}] driver shutdown completely";
    inline constexpr std::string_view kLmsInitializedInstTpl = "LiDAR instance [{}] initialized successfully";
    inline constexpr std::string_view kLmsShutdownInstTpl = "LiDAR instance [{}] driver shutdown completely";

    // Rig-level diagnostics. Only a finalized manifest establishes the result.
    inline constexpr std::string_view kStartingDrivers = "Starting Amiga Drivers";
    inline constexpr std::string_view kReceivedSignalTpl = "Received signal {}, shutting down all drivers...";
    inline constexpr std::string_view kAllDriversShutDown = "All drivers shut down";

    // Per-driver failure diagnostics emitted by the session coordinator.
    inline constexpr std::string_view kAsterxInitFailed = "AsteRx driver initialization failed";
    inline constexpr std::string_view kFx10InitFailed = "FX10 driver initialization failed";
    inline constexpr std::string_view kGoxInitFailed = "GoX driver initialization failed";
    inline constexpr std::string_view kLms4xxxInitFailed = "LMS4xxx driver initialization failed";
    // Appended to a driver name by the no-data watchdog. The corresponding
    // sensor slot is also marked failed in the structured lifecycle document.
    inline constexpr std::string_view kGuardNoDataSuffix = " driver stopped: no data";

    inline constexpr std::string_view kAsterxRunException = "AsteRx run() exception";
    inline constexpr std::string_view kFx10RunException = "FX10 run() exception";
    inline constexpr std::string_view kGoxRunException = "GoX run() exception";
    inline constexpr std::string_view kLms4xxxRunException = "LMS4xxx run() exception";
} // namespace common::Markers
