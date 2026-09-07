#pragma once

#include <cstdint>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>


namespace common {
    namespace Logger {
        struct Config {
            std::string log_file; // Empty = no file logging
            bool quiet = false; // Suppress below-warn messages from the console
        };

        void Init(const Config &config, const std::string &logger_name = "AmigaDriver");

        // Console (stderr) messages dropped because the pipe reader did not keep up.
        // Only ever non-zero when stderr is not a terminal (nonblocking_stderr_sink.h);
        // the file log is always complete
        std::uint64_t ConsoleLinesDropped();
    } // namespace Logger


    namespace Log {
        // Log at the given level; never throws
        void LogMessage(spdlog::level::level_enum level, std::string_view module, std::string_view msg,
                         std::string_view error_detail = "");

        // Log at error level; throws std::runtime_error unless throw_error=false.
        // The ONLY throwing path of the logging API — reserve it for init/config
        // code where aborting is intended, never in worker threads or destructors.
        void LogAndThrow(std::string_view module, std::string_view msg, std::string_view error_detail = "",
                           bool throw_error = true);

        // Pre-log callback: invoked before each console log write
        using PreLogCallback = void (*)();

        void SetPreLogCallback(PreLogCallback cb);

        // Invoke the registered pre-log callback (no-op when none registered)
        void RunPreLogCallback();
    } // namespace Log


    class DriverLog {
    public:
        DriverLog() = default;

        explicit DriverLog(std::string module, spdlog::level::level_enum min_level = spdlog::level::trace)
            : module_(std::move(module)), min_level_(min_level) {
        }

        // NOT thread-safe: call once before the driver's threads exist
        void Configure(std::string module, spdlog::level::level_enum min_level) {
            module_ = std::move(module);
            min_level_ = min_level;
        }

        template<typename... Args>
        void Log(spdlog::level::level_enum lvl, spdlog::format_string_t<Args...> f, Args &&... args) {
            if (lvl < min_level_) {
                return;
            }
            Log::LogMessage(lvl, module_, fmt::format(f, std::forward<Args>(args)...));
        }

        template<typename... A>
        void Trace(spdlog::format_string_t<A...> f, A &&... a) { Log(spdlog::level::trace, f, std::forward<A>(a)...); }

        template<typename... A>
        void Debug(spdlog::format_string_t<A...> f, A &&... a) { Log(spdlog::level::debug, f, std::forward<A>(a)...); }

        template<typename... A>
        void Info(spdlog::format_string_t<A...> f, A &&... a) { Log(spdlog::level::info, f, std::forward<A>(a)...); }

        template<typename... A>
        void Warn(spdlog::format_string_t<A...> f, A &&... a) { Log(spdlog::level::warn, f, std::forward<A>(a)...); }

        template<typename... A>
        void Error(spdlog::format_string_t<A...> f, A &&... a) { Log(spdlog::level::err, f, std::forward<A>(a)...); }

        // Throwing variant, *_driver_app layer only: logs, then throws std::runtime_error(message)
        // when `throw_error` (even if filtered by min_level). The bool is SFINAE-constrained so a
        // plain Error("...") never lands here
        template<typename B, typename... A,
            std::enable_if_t<std::is_same_v<std::remove_cv_t<std::remove_reference_t<B> >, bool>, int> = 0>
        void Error(B throw_error, spdlog::format_string_t<A...> f, A &&... a) {
            const std::string msg = fmt::format(f, std::forward<A>(a)...);
            if (spdlog::level::err >= min_level_) {
                Log::LogMessage(spdlog::level::err, module_, msg);
            }
            if (throw_error) {
                throw std::runtime_error(msg);
            }
        }

        template<typename... A>
        void Critical(spdlog::format_string_t<A...> f, A &&... a) {
            Log(spdlog::level::critical, f, std::forward<A>(a)...);
        }

    private:
        std::string module_{"Driver"};
        spdlog::level::level_enum min_level_{spdlog::level::trace};
    };
} // namespace common
