#include "logger.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <stdexcept>
#include <vector>

#include "nonblocking_stderr_sink.h"


namespace {
    // Single process-wide slot; only this translation unit touches it.
    std::atomic<common::Log::PreLogCallback> g_pre_log_cb{nullptr};
}


namespace common {
    namespace Logger {
        void Init(const Config &config, const std::string &logger_name) {
            // A terminal gets colours and may block (a human is reading); a pipe or
            // file (the GUI, docker exec) gets the non-blocking sink: a stalled
            // reader must never stall an acquisition thread through a log call
            spdlog::sink_ptr console_sink;
            if (::isatty(STDERR_FILENO)) {
                auto color = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
                color->set_pattern("%^[%H:%M:%S] [%l] %v%$");
                console_sink = color;
            } else {
                auto plain = std::make_shared<NonBlockingStderrSink>();
                plain->set_pattern("[%H:%M:%S] [%l] %v");
                console_sink = plain;
            }
            console_sink->set_level(config.quiet ? spdlog::level::warn : spdlog::level::trace);

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

            // err+ flush on the calling thread (crash tail, GUI health); the rest every 200 ms
            logger->flush_on(spdlog::level::err);
            spdlog::set_default_logger(logger);
            spdlog::flush_every(std::chrono::milliseconds(200));
        }


        std::uint64_t ConsoleLinesDropped() { return NonBlockingStderrSink::DroppedCount(); }
    } // namespace Logger


    namespace Log {
        void SetPreLogCallback(PreLogCallback cb) {
            g_pre_log_cb.store(cb, std::memory_order_release);
        }


        void RunPreLogCallback() {
            if (auto cb = g_pre_log_cb.load(std::memory_order_acquire)) {
                cb();
            }
        }


        void LogMessage(spdlog::level::level_enum level, std::string_view module, std::string_view msg,
                         std::string_view error_detail) {
            if (auto cb = g_pre_log_cb.load(std::memory_order_acquire)) {
                cb();
            }
            if (error_detail.empty()) {
                spdlog::log(level, "[{}]: {}", module, msg);
            } else {
                spdlog::log(level, "[{}]: {} - {}", module, msg, error_detail);
            }
        }


        void LogAndThrow(std::string_view module, std::string_view msg, std::string_view error_detail,
                           bool throw_error) {
            LogMessage(spdlog::level::err, module, msg, error_detail);
            if (throw_error) {
                throw std::runtime_error(std::string(msg));
            }
        }
    } // namespace Log
} // namespace common
