/// @file test_logger.cpp
/// @brief Regression tests for the GUI log contract: every file-log line must
/// match app/services/log_buffer.py LINE_RE, and the throw semantics
/// documented in logger.h must hold (log_and_throw is the ONLY throwing path).

#include <doctest/doctest.h>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "driver_markers.h"
#include "logger.h"


namespace {
    // Mirrors app/services/log_buffer.py LINE_RE — the GUI's file-log parser
    const std::regex kLineRe(R"(^\[(\d{2}:\d{2}:\d{2})\] \[(\w+)\] \[([^\]]+)\]: (.*)$)");

    // spdlog %l level tokens the GUI accepts (log_buffer.py LEVELS)
    const std::vector<std::string> kLevels = {"trace", "debug", "info", "warning", "error", "critical"};

    std::filesystem::path MakeLogPath(const std::string &tag) {
        const auto path = std::filesystem::temp_directory_path() / ("common_tests_" + tag + ".log");
        std::filesystem::remove(path);
        return path;
    }

    std::vector<std::string> ReadLines(const std::filesystem::path &path) {
        spdlog::default_logger()->flush();
        std::ifstream in(path);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    bool LevelTokenValid(const std::string &token) {
        for (const auto &lvl: kLevels) {
            if (token == lvl) {
                return true;
            }
        }
        return false;
    }
} // namespace


TEST_CASE(

    "file log lines match the GUI LINE_RE contract"
) {
    const auto path = MakeLogPath("format");
    common::Logger::Init({path.string(), /*quiet=*/true}, "common_tests_format");

    common::Log::LogMessage(spdlog::level::trace, common::Markers::kModuleMain, "trace message");
    common::Log::LogMessage(spdlog::level::info, common::Markers::kModuleGox, common::Markers::kGoxInitialized);
    common::Log::LogMessage(spdlog::level::warn, common::Markers::kModuleLms4xxx, "with detail", "detail text");
    common::Log::LogAndThrow(common::Markers::kModuleMain, "boom", "", /*throw_error=*/false);

    const auto lines = ReadLines(path);
    REQUIRE(lines.size() == 4);

    std::smatch m;
    for (const auto &line: lines) {
        CAPTURE(line);
        REQUIRE(std::regex_match(line, m, kLineRe));
        CHECK(LevelTokenValid(m[2].str()));
    }

    // Spot-check module token and message routing of the marker line
    REQUIRE(std::regex_match(lines[1], m, kLineRe));
    CHECK(m[2].str() == "info");
    CHECK(m[3].str() == std::string(common::Markers::kModuleGox));
    CHECK(m[4].str() == std::string(common::Markers::kGoxInitialized));

    // The " - " detail suffix stays inside the msg group
    REQUIRE(std::regex_match(lines[2], m, kLineRe));
    CHECK(m[4].str() == "with detail - detail text");
}


TEST_CASE(

    "throw semantics (logger.h contract)"
) {
    const auto path = MakeLogPath("throw");
    common::Logger::Init({path.string(), /*quiet=*/true}, "common_tests_throw");

    // log_message is pure logging at ANY level — never throws
    CHECK_NOTHROW(common::Log::LogMessage(spdlog::level::err, "Main", "boom"));
    CHECK_NOTHROW(common::Log::LogMessage(spdlog::level::critical, "Main", "boom"));
    // log_and_throw is the only throwing path, and only when asked
    CHECK_THROWS_AS(common::Log::LogAndThrow("Main", "boom"), std::runtime_error);
    CHECK_NOTHROW(common::Log::LogAndThrow("Main", "boom", "", /*throw_error=*/false));
    // DriverLog never throws by default (safe in threads/destructors/Qt slots)
    common::DriverLog Log("AsteRx");
    CHECK_NOTHROW(Log.Error("disk write failed: {}", 42));
    CHECK_NOTHROW(Log.Critical("fatal but non-throwing: {}", "reason"));
    // The explicit Error(bool, ...) overload throws only when asked; the thrown
    // message is the formatted text. Reserved for the *_driver_app layer.
    CHECK_NOTHROW(Log.Error(false, "recoverable: {}", 1));
    CHECK_THROWS_AS(Log.Error(true, "fatal: {}", 2), std::runtime_error);
    try {
        Log.Error(true, "fatal: {}", 3);
    } catch (const std::runtime_error &e) {
        CHECK(std::string(e.what()) == "fatal: 3");
    }
    // A leading string literal must resolve to the non-throwing overload, never
    // via const char* -> bool conversion (SFINAE-guarded).
    CHECK_NOTHROW(Log.Error("plain message, no args"));
}


TEST_CASE(

    "DriverLog lines carry the [module]: prefix and pass LINE_RE"
) {
    const auto path = MakeLogPath("driverlog");
    common::Logger::Init({path.string(), /*quiet=*/true}, "common_tests_driverlog");

    common::DriverLog Log("AsteRx", spdlog::level::info);
    Log.Debug("filtered out by min_level {}", 1);
    Log.Info("hello {}", 1);
    Log.Error("oops {}", 2);
    Log.Critical("fatal {}", 3);

    const auto lines = ReadLines(path);
    REQUIRE(lines.size() == 3); // debug filtered by the driver-level min_level

    std::smatch m;
    REQUIRE(std::regex_match(lines[0], m, kLineRe));
    CHECK(m[2].str() == "info");
    CHECK(m[3].str() == "AsteRx");
    CHECK(m[4].str() == "hello 1");

    // Level tokens pass through unchanged — critical stays [critical]
    REQUIRE(std::regex_match(lines[1], m, kLineRe));
    CHECK(m[2].str() == "error");
    CHECK(m[4].str() == "oops 2");
    REQUIRE(std::regex_match(lines[2], m, kLineRe));
    CHECK(m[2].str() == "critical");
    CHECK(m[4].str() == "fatal 3");
}


TEST_CASE(

    "error lines reach the file without an explicit flush"
) {
    // The web GUI tails this file from ANOTHER process, so it cannot flush it;
    // and an abort() would take an unflushed buffer with it, losing exactly the
    // crash tail. spdlog flushes nothing by default (flush_level_ = off), hence
    // the flush_on(err) in Logger::Init. Info/trace lines are covered by the
    // background spdlog::flush_every() and are deliberately not asserted here.
    const auto path = MakeLogPath("flush");
    common::Logger::Init({path.string(), /*quiet=*/true}, "common_tests_flush");

    common::Log::LogMessage(spdlog::level::err, common::Markers::kModuleMain, "reaches disk at once");

    // NOTE: read WITHOUT ReadLines() — that helper flushes, which is the very
    // thing this test must not do.
    std::ifstream in(path);
    std::string line;
    REQUIRE(std::getline(in, line));
    CHECK(line.find("reaches disk at once") != std::string::npos);
}


TEST_CASE(

    "a stalled stderr reader never blocks logging (non-blocking console sink)"
) {
    // The GUI holds the process's stderr pipe. Fill such a pipe and never read
    // it: every log call must still return at once, the dropped lines must be
    // counted, and the file log must stay complete.
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const int saved_stderr = ::dup(STDERR_FILENO);
    REQUIRE(saved_stderr >= 0);
    REQUIRE(::dup2(fds[1], STDERR_FILENO) == STDERR_FILENO);

    const auto path = MakeLogPath("nonblock");
    common::Logger::Init({path.string(), /*quiet=*/false}, "common_tests_nonblock"); // stderr is a pipe now
    const auto dropped_before = common::Logger::ConsoleLinesDropped();

    constexpr int kLines = 4000; // ~80 B each: well past the 64 KiB pipe capacity
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kLines; ++i) {
        common::Log::LogMessage(spdlog::level::info, common::Markers::kModuleMain,
                                "filler line for a pipe nobody reads, number " + std::to_string(i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Restore stderr before any assertion can print
    ::dup2(saved_stderr, STDERR_FILENO);
    ::close(saved_stderr);
    ::close(fds[0]);
    ::close(fds[1]);

    CHECK(elapsed < std::chrono::seconds(2));
    CHECK(common::Logger::ConsoleLinesDropped() > dropped_before);
    CHECK(ReadLines(path).size() >= static_cast<std::size_t>(kLines)); // the file sink saw everything
}


TEST_CASE(

    "marker templates render the exact GUI-matched strings"
) {
    CHECK(fmt::format(fmt::runtime(common::Markers::kLmsInitializedInstTpl), "front")
        == "LiDAR instance [front] initialized successfully");
    CHECK(fmt::format(fmt::runtime(common::Markers::kLmsShutdownInstTpl), "front")
        == "LiDAR instance [front] driver shutdown completely");
    CHECK(fmt::format(fmt::runtime(common::Markers::kReceivedSignalTpl), 15)
        == "Received signal 15, shutting down all drivers...");

    // The [Main] failure markers must factor into <name> + fixed suffix — the
    // GUI maps them to sensors by leading name and detects kind by suffix
    const std::string init_suffix = " driver initialization failed";
    const std::string run_suffix = " run() exception";
    CHECK(std::string(common::Markers::kAsterxInitFailed) == "AsteRx" + init_suffix);
    CHECK(std::string(common::Markers::kGoxInitFailed) == "GoX" + init_suffix);
    CHECK(std::string(common::Markers::kLms4xxxInitFailed) == "LMS4xxx" + init_suffix);
    CHECK(std::string(common::Markers::kAsterxRunException) == "AsteRx" + run_suffix);
    CHECK(std::string(common::Markers::kGoxRunException) == "GoX" + run_suffix);
    CHECK(std::string(common::Markers::kLms4xxxRunException) == "LMS4xxx" + run_suffix);
}
