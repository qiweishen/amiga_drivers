#include "capture_runner.hpp"

#include <chrono>
#include <filesystem>
#include <sys/statvfs.h>
#include <thread>

#include "logger.h"
#include "stats.hpp"
#include "util.hpp"
#include "ebus/camera_session.hpp"
#include "driver_markers.h"

namespace jai {
    namespace {
        Common::DriverLog g_log{"GoX"};


        uint64_t free_disk_bytes(const std::string &path) {
            struct statvfs vfs{};
            if (::statvfs(path.c_str(), &vfs) != 0) {
                return 0;
            }
            return static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        }


        std::string basename_of(const std::string &path) {
            const size_t pos = path.find_last_of('/');
            return pos == std::string::npos ? path : path.substr(pos + 1);
        }
    } // namespace


    CaptureRunner::CaptureRunner(AppConfig cfg, StopController *stop) : cfg_(std::move(cfg)), stop_(stop) {
    }


    CaptureRunner::~CaptureRunner() {
        if (initialized_ && !shutdown_done_ && !stop_->stop_requested()) {
            // Reached only via stack unwinding: an exception escaped between
            // init() and shutdown() (e.g. out of the poll loop). Record the
            // stop as an error so the reported outcome agrees with the fatal path.
            stop_->request_stop(StopReason::Error);
        }
        try {
            shutdown();
        } catch (...) {
            // Never throw out of a destructor (may run during unwinding).
        }
    }

    bool CaptureRunner::init(const std::string &session_dir_override) {
        gen_uuid_v4(session_uuid_);
        start_rt_ = now_realtime_ns();
        if (session_dir_override.empty()) {
            session_name_ = compact_utc(start_rt_) + "_" + hex_prefix(session_uuid_, 3);
            session_dir_ = cfg_.output.output_dir + "/" + session_name_;
        } else {
            // Unified mode: the host application owns the output layout.
            session_dir_ = session_dir_override;
            session_name_ = basename_of(session_dir_override);
        }
        std::error_code ec;
        std::filesystem::create_directories(session_dir_, ec);
        if (ec) {
            g_log.error("Cannot create output directory: {}", ec.message());
            return false;
        }

        for (size_t i = 0; i < cfg_.cameras.size(); ++i) {
            if (cfg_.cameras[i].enabled) {
                sessions_.push_back(std::make_unique<ebus::CameraSession>(static_cast<uint32_t>(i), cfg_.cameras[i],
                                                                          cfg_, session_uuid_, stop_));
            }
        }
        for (auto &session: sessions_) {
            try {
                session->start(session_dir_);
            } catch (const std::exception &e) {
                // A stop that landed mid-bring-up (Ctrl+C during a PTP wait, or
                // a running camera's writer failing) surfaces as an exception
                // out of whatever step was active — report the concrete cause.
                const bool interrupted = stop_->stop_requested() &&
                                         (stop_->reason() == StopReason::Signal || stop_->reason() ==
                                          StopReason::External);
                last_error_ = interrupted ? std::string("interrupted during bring-up: ") + e.what() : e.what();
                g_log.error("Startup failed: {}", last_error_);
                stop_->request_stop(StopReason::Error);
                for (auto &s: sessions_) {
                    s->stop_and_join();
                }
                return false;
            }
            if (stop_->stop_requested()) {
                break; // e.g. Ctrl+C during a PTP wait
            }
        }

        initialized_ = true;
        return true;
    }


    void CaptureRunner::monitor_loop(const std::function<bool()> &external_stop) {
        if (!initialized_) {
            return;
        }

        // max_duration_s measures CAPTURE time
        capture_start_mono_ = now_monotonic_ns();
        const double stats_interval_s = cfg_.stats_interval_s;
        const double offset_interval_s = cfg_.ptp.offset_report_interval_s;
        uint64_t last_stats_mono = capture_start_mono_;
        uint64_t last_offset_mono = capture_start_mono_;
        while (!stop_->stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (external_stop && external_stop()) {
                stop_->request_stop(StopReason::External);
                break;
            }
            const uint64_t now_mono = now_monotonic_ns();
            const uint64_t uptime_s = (now_mono - capture_start_mono_) / 1000000000ull;

            if (cfg_.output.max_duration_s > 0 &&
                static_cast<double>(now_mono - capture_start_mono_) >= cfg_.output.max_duration_s * 1e9) {
                g_log.info("max_duration_s ({}) reached", cfg_.output.max_duration_s);
                stop_->request_stop(StopReason::LimitReached);
                break;
            }
            if (stats_interval_s > 0 && static_cast<double>(now_mono - last_stats_mono) >= stats_interval_s * 1e9) {
                const double actual = static_cast<double>(now_mono - last_stats_mono) / 1e9;
                last_stats_mono = now_mono;
                const uint64_t free_bytes = free_disk_bytes(session_dir_);
                for (auto &s: sessions_) {
                    s->poll_stream_stats();
                    g_log.info("{}", s->reporter().periodic_line(actual, uptime_s, free_bytes));
                }
            }
            if (offset_interval_s > 0 && static_cast<double>(now_mono - last_offset_mono) >= offset_interval_s * 1e9) {
                last_offset_mono = now_mono;
                for (auto &s: sessions_) {
                    s->refresh_ptp_offset();
                }
            }
        }
    }


    bool CaptureRunner::shutdown() {
        if (shutdown_done_) {
            return clean_;
        }
        shutdown_done_ = true;
        if (!initialized_) {
            // init() already tore down, logged and kept the concrete error.
            clean_ = false;
            return clean_;
        }

        g_log.info("stopping (reason: {})", stop_reason_name(stop_->reason()));
        for (auto &s: sessions_) {
            s->stop_and_join();
        }
        const uint64_t uptime_s = capture_start_mono_ != 0
                                      ? (now_monotonic_ns() - capture_start_mono_) / 1000000000ull
                                      : 0;
        bool all_clean = true;
        for (auto &s: sessions_) {
            g_log.info("{}", s->reporter().final_summary(uptime_s));
            all_clean = all_clean && s->clean();
        }

        if (stop_->reason() == StopReason::Error) {
            if (last_error_.empty()) {
                last_error_ = "session stopped on an error (see the log above for the concrete cause)";
            }
            clean_ = false;
        } else if (!all_clean) {
            last_error_ = "frame drops / incomplete frames / stream errors detected (see the final statistics above)";
            clean_ = false;
        }
        return clean_;
    }
} // namespace jai
