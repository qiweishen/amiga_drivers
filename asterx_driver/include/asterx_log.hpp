// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Logging facade for the asterx core code (session / sbf_writer): a thin
// prefix/level shim over the process-wide spdlog default logger. The wrapper
// calls configure() once — before the Qt thread starts — to set the
// "[AsteRx]: " prefix, the spinner pre-log hook and the level filter.
// Deliberately NOT routed through Common::Log::log_message: error-level
// messages there throw, which is forbidden inside Qt slots.

#include <functional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace asterx::log {
    namespace detail {
        struct State {
            std::string prefix; // e.g. "[AsteRx]: "
            std::function<void()> pre_log; // runs before each emitted message
            spdlog::level::level_enum min_level{spdlog::level::trace};
        };

        inline State &state() {
            static State s;
            return s;
        }
    } // namespace detail

    // NOT thread-safe: call once before the Qt worker thread exists;
    // read-only afterwards.
    inline void configure(std::string prefix, std::function<void()> pre_log,
                          spdlog::level::level_enum min_level) {
        detail::state() = {std::move(prefix), std::move(pre_log), min_level};
    }

    template<typename... Args>
    void log(spdlog::level::level_enum lvl, spdlog::format_string_t<Args...> f, Args &&... args) {
        const auto &s = detail::state();
        if (lvl < s.min_level) {
            return;
        }
        if (s.pre_log) {
            s.pre_log();
        }
        if (s.prefix.empty()) {
            spdlog::log(lvl, f, std::forward<Args>(args)...);
        } else {
            spdlog::log(lvl, "{}{}", s.prefix, fmt::format(f, std::forward<Args>(args)...));
        }
    }

    template<typename... A>
    void trace(spdlog::format_string_t<A...> f, A &&... a) { log(spdlog::level::trace, f, std::forward<A>(a)...); }

    template<typename... A>
    void debug(spdlog::format_string_t<A...> f, A &&... a) { log(spdlog::level::debug, f, std::forward<A>(a)...); }

    template<typename... A>
    void info(spdlog::format_string_t<A...> f, A &&... a) { log(spdlog::level::info, f, std::forward<A>(a)...); }

    template<typename... A>
    void warn(spdlog::format_string_t<A...> f, A &&... a) { log(spdlog::level::warn, f, std::forward<A>(a)...); }

    template<typename... A>
    void error(spdlog::format_string_t<A...> f, A &&... a) { log(spdlog::level::err, f, std::forward<A>(a)...); }

    template<typename... A>
    void critical(spdlog::format_string_t<A...> f, A &&... a) { log(spdlog::level::critical, f, std::forward<A>(a)...); }
} // namespace asterx::log
