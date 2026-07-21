/// @file test_logger.cpp
/// @brief Regression tests for the GUI log contract: every file-log line must
/// match app/services/log_buffer.py LINE_RE, and the throw semantics
/// documented in logger.h must hold (log_and_throw is the ONLY throwing path).

#include <doctest/doctest.h>

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
	const std::vector<std::string> kLevels = { "trace", "debug", "info", "warning", "error", "critical" };

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
}  // namespace


TEST_CASE("file log lines match the GUI LINE_RE contract") {
	const auto path = MakeLogPath("format");
	Common::Logger::init({ path.string(), /*quiet=*/true }, "common_tests_format");

	Common::Log::log_message(spdlog::level::trace, Common::Markers::kModuleMain, "trace message");
	Common::Log::log_message(spdlog::level::info, Common::Markers::kModuleGox, Common::Markers::kGoxInitialized);
	Common::Log::log_message(spdlog::level::warn, Common::Markers::kModuleLms4xxx, "with detail", "detail text");
	Common::Log::log_and_throw(Common::Markers::kModuleMain, "boom", "", /*throw_error=*/false);

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
	CHECK(m[3].str() == std::string(Common::Markers::kModuleGox));
	CHECK(m[4].str() == std::string(Common::Markers::kGoxInitialized));

	// The " - " detail suffix stays inside the msg group
	REQUIRE(std::regex_match(lines[2], m, kLineRe));
	CHECK(m[4].str() == "with detail - detail text");
}


TEST_CASE("throw semantics (logger.h contract)") {
	const auto path = MakeLogPath("throw");
	Common::Logger::init({ path.string(), /*quiet=*/true }, "common_tests_throw");

	// log_message is pure logging at ANY level — never throws
	CHECK_NOTHROW(Common::Log::log_message(spdlog::level::err, "Main", "boom"));
	CHECK_NOTHROW(Common::Log::log_message(spdlog::level::critical, "Main", "boom"));
	// log_and_throw is the only throwing path, and only when asked
	CHECK_THROWS_AS(Common::Log::log_and_throw("Main", "boom"), std::runtime_error);
	CHECK_NOTHROW(Common::Log::log_and_throw("Main", "boom", "", /*throw_error=*/false));
	// DriverLog never throws (safe in threads/destructors/Qt slots)
	Common::DriverLog log("AsteRx");
	CHECK_NOTHROW(log.error("disk write failed: {}", 42));
	CHECK_NOTHROW(log.critical("fatal but non-throwing: {}", "reason"));
}


TEST_CASE("DriverLog lines carry the [module]: prefix and pass LINE_RE") {
	const auto path = MakeLogPath("driverlog");
	Common::Logger::init({ path.string(), /*quiet=*/true }, "common_tests_driverlog");

	Common::DriverLog log("AsteRx", spdlog::level::info);
	log.debug("filtered out by min_level {}", 1);
	log.info("hello {}", 1);
	log.error("oops {}", 2);
	log.critical("fatal {}", 3);

	const auto lines = ReadLines(path);
	REQUIRE(lines.size() == 3);	 // debug filtered by the driver-level min_level

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


TEST_CASE("marker templates render the exact GUI-matched strings") {
	CHECK(fmt::format(fmt::runtime(Common::Markers::kLmsInitializedTpl), "front")
		  == "LiDAR instance [front] initialized successfully");
	CHECK(fmt::format(fmt::runtime(Common::Markers::kLmsShutdownTpl), "front")
		  == "LiDAR instance [front] driver shutdown completely");
	CHECK(fmt::format(fmt::runtime(Common::Markers::kReceivedSignalTpl), 15)
		  == "Received signal 15, shutting down all drivers...");

	// The [Main] failure markers must factor into <name> + fixed suffix — the
	// GUI maps them to sensors by leading name and detects kind by suffix
	const std::string init_suffix = " driver initialization failed";
	const std::string run_suffix = " run() exception";
	CHECK(std::string(Common::Markers::kAsterxInitFailed) == "AsteRx" + init_suffix);
	CHECK(std::string(Common::Markers::kGoxInitFailed) == "GoX" + init_suffix);
	CHECK(std::string(Common::Markers::kIns401InitFailed) == "INS401" + init_suffix);
	CHECK(std::string(Common::Markers::kLms4xxxInitFailed) == "LMS4xxx" + init_suffix);
	CHECK(std::string(Common::Markers::kAsterxRunException) == "AsteRx" + run_suffix);
	CHECK(std::string(Common::Markers::kGoxRunException) == "GoX" + run_suffix);
	CHECK(std::string(Common::Markers::kIns401RunException) == "INS401" + run_suffix);
	CHECK(std::string(Common::Markers::kLms4xxxRunException) == "LMS4xxx" + run_suffix);
}
