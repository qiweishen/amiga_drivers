#include "run_guards.h"

#include <spdlog/fmt/fmt.h>


namespace common {
    double FreeSpaceGiB(const std::filesystem::path &path) {
        std::error_code ec;
        const auto info = std::filesystem::space(path, ec);
        if (ec || info.available == static_cast<std::uintmax_t>(-1)) {
            return -1.0;
        }
        return static_cast<double>(info.available) / static_cast<double>(1ull << 30);
    }


    RunGuards::Result RunGuards::CheckDisk(const double free_gib) {
        if (free_gib < 0.0) {
            return {}; // unreadable, not full
        }

        if (cfg_.disk_min_free_gib > 0.0 && free_gib < cfg_.disk_min_free_gib) {
            return {
                Verdict::kStop,
                fmt::format("Disk free {:.1f} GiB is below the hard floor of {:.1f} GiB", free_gib,
                            cfg_.disk_min_free_gib)
            };
        }

        if (cfg_.disk_warn_free_gib > 0.0 && free_gib < cfg_.disk_warn_free_gib) {
            if (disk_warn_latched_) {
                return {};
            }
            disk_warn_latched_ = true;
            return {
                Verdict::kWarn,
                fmt::format("Disk free {:.1f} GiB is below the warning floor of {:.1f} GiB", free_gib,
                            cfg_.disk_warn_free_gib)
            };
        }

        disk_warn_latched_ = false; // back above the soft floor: re-arm
        return {};
    }


    RunGuards::Result RunGuards::CheckSensor(const std::string_view name, const std::optional<std::uint64_t> silent_us) {
        const auto latch = [this, name]() -> bool & {
            const auto it = sensor_warn_latched_.find(name);
            if (it != sensor_warn_latched_.end()) {
                return it->second;
            }
            return sensor_warn_latched_.emplace(std::string{name}, false).first->second;
        };

        if (!silent_us.has_value()) {
            latch() = false; // silence is expected: drop any pending warning
            return {};
        }

        const double silent_s = static_cast<double>(*silent_us) / 1e6;

        if (cfg_.no_data_abort_s > 0.0 && silent_s >= cfg_.no_data_abort_s) {
            return {
                Verdict::kStop,
                fmt::format("silent for {:.1f} s, watchdog abort threshold {:.1f} s", silent_s, cfg_.no_data_abort_s)
            };
        }

        if (cfg_.no_data_warn_s > 0.0 && silent_s >= cfg_.no_data_warn_s) {
            if (latch()) {
                return {};
            }
            latch() = true;
            return {
                Verdict::kWarn,
                fmt::format("has delivered no data for {:.1f} s", silent_s)
            };
        }

        latch() = false; // data resumed: re-arm
        return {};
    }
} // namespace common
