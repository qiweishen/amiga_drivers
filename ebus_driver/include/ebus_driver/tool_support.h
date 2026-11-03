#pragma once

// Standalone tools share orchestration only; SDK receive/stop and RAW writing
// always go through EbusCameraDriver, including interrupted/partial starts.
#include "ebus_driver/camera_driver.h"
#include "string_util.h"
#include "time_util.h"

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>


namespace ebus_driver::tool {
    inline volatile std::sig_atomic_t signal = 0;
    inline void OnSignal(int value) { signal = value; }

    inline void InstallSignals() {
        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);
    }

    inline bool ParseDouble(const char *text, double &value) {
        if (!text || !*text) {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        value = std::strtod(text, &end);
        return errno == 0 && end != text && *end == '\0' && std::isfinite(value);
    }

    inline bool ParseCount(const char *text, std::uint64_t &value) {
        if (!text || !*text || *text == '-') {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        value = std::strtoull(text, &end, 10);
        return errno == 0 && end != text && *end == '\0';
    }

    inline int SnapshotFail(int code, const std::string &reason) {
        std::printf("SNAPSHOT: FAIL %d %s\n", code, common::StringUtil::OneLine(reason).c_str());
        std::fflush(stdout);
        return code;
    }

    inline std::filesystem::path TimestampedPath(const std::filesystem::path &parent, const char *stem) {
        const auto now = common::TimeUtil::RealtimeNowNs();
        return std::filesystem::absolute(parent) /
               (std::string(stem) + "_" + common::TimeUtil::CompactUtc(now) + "_" +
                std::to_string(now % 1000000000ull));
    }

    inline std::uint64_t Count(const nlohmann::ordered_json &statistics, const char *key) {
        if (statistics.contains(key)) {
            return statistics.at(key).get<std::uint64_t>();
        }
        if (statistics.contains("recording") && statistics.at("recording").is_object()) {
            return statistics.at("recording").value(key, std::uint64_t{0});
        }
        return 0;
    }

    inline int RunSnapshot(CameraDefinition definition, std::uint64_t requested_frames) {
        InstallSignals();
        EbusCameraDriver driver(std::move(definition));
        bool starved = false;
        bool started = false;
        std::string exception;
        try {
            started = driver.Init([&] {
                const auto idle = driver.MicrosSinceLastData();
                if (idle && *idle >= 10000000ull) {
                    starved = true;
                }
                return signal != 0 || starved;
            });
            if (started) {
                driver.Run();
            }
        } catch (const std::exception &error) {
            exception = error.what();
        }
        try {
            driver.Shutdown();
        } catch (const std::exception &error) {
            exception = error.what();
        }
        const auto statistics = driver.FinalStatistics();
        const auto written = Count(statistics, "frames_written");
        const auto error = exception.empty() ? statistics.value("error", std::string()) : exception;
        if (signal != 0) {
            return SnapshotFail(130, "interrupted");
        }
        if (!started) {
            return SnapshotFail(3, error.empty() ? "camera initialization failed" : error);
        }
        if (written == 0) {
            return SnapshotFail(7, starved ? "no-frame (no data for 10 s)" : "no-frame; " + error);
        }
        if (starved || driver.HasFailed() || !exception.empty() || written < requested_frames) {
            return SnapshotFail(5, "incomplete snapshot; " + error);
        }
        std::printf("SNAPSHOT: OK %s\n", driver.Directory().string().c_str());
        std::fflush(stdout);
        return 0;
    }
} // namespace ebus_driver::tool
