#include "logger.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <stdexcept>
#include <vector>


namespace {
    // Single process-wide slot; only this translation unit touches it.
    std::atomic<Common::Log::PreLogCallback> g_pre_log_cb{nullptr};
}


namespace Common {
    namespace Logger {
        void init(const Config &config, const std::string &logger_name) {
            auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            // Quiet mode: suppress INFO-level messages from the console (WARN+ still shown)
            // TODO: Debug
            console_sink->set_level(config.quiet ? spdlog::level::warn : spdlog::level::trace);
            console_sink->set_pattern("%^[%H:%M:%S] [%l] %v%$");

            std::vector<spdlog::sink_ptr> sinks{console_sink};

            if (!config.log_file.empty()) {
                auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.log_file, false);
                file_sink->set_level(spdlog::level::trace);
                file_sink->set_pattern("[%H:%M:%S] [%l] %v");
                sinks.push_back(file_sink);
            }

            // The process-wide single spdlog instance: every driver logs through
            // the default logger (console at info, file at trace)
            auto logger = std::make_shared<spdlog::logger>(logger_name, sinks.begin(), sinks.end());
            logger->set_level(spdlog::level::trace);

            // Flush policy. spdlog flushes NOTHING by default (flush_level_ is
            // level::off, which no message can reach), so the file sink's stdio
            // buffer held ~4 KiB — at this project's ~450 B/s that is ~9 s of
            // log stuck in memory. Two consequences, both bad: the web GUI tails
            // this file for per-sensor health and so ran that far behind, and an
            // abort() lost exactly the last lines a post-mortem needs.
            //   - err and above flush on the calling thread: rare, and they are
            //     the crash tail plus what drives the GUI health machine.
            //   - everything else (statistics, trace) is flushed by spdlog's
            //     background thread, so the real-time acquisition threads never
            //     pay for a write() syscall of their own.
            logger->flush_on(spdlog::level::err);
            spdlog::set_default_logger(logger);
            spdlog::flush_every(std::chrono::milliseconds(200));
        }
    } // namespace Logger


    namespace Log {
        void set_pre_log_callback(PreLogCallback cb) {
            g_pre_log_cb.store(cb, std::memory_order_release);
        }


        void run_pre_log_callback() {
            if (auto cb = g_pre_log_cb.load(std::memory_order_acquire)) {
                cb();
            }
        }


        void log_message(spdlog::level::level_enum level, std::string_view module,
                         std::string_view msg, std::string_view error_detail, bool throw_error) {
            // Pure logging at ANY level — never throws. Paths that must abort
            // use log_and_throw(..., throw_error=true) explicitly.
            if (auto cb = g_pre_log_cb.load(std::memory_order_acquire)) {
                cb();
            }
            if (error_detail.empty()) {
                spdlog::log(level, "[{}]: {}", module, msg);
            } else {
                spdlog::log(level, "[{}]: {} - {}", module, msg, error_detail);
            }
        }


        void log_and_throw(std::string_view module, std::string_view msg, std::string_view error_detail,
                           bool throw_error) {
            log_message(spdlog::level::err, module, msg, error_detail);
            if (throw_error) {
                throw std::runtime_error(std::string(msg));
            }
        }
    } // namespace Log
} // namespace Common
