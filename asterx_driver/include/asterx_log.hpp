#pragma once

#include "logger.h"

#include <spdlog/spdlog.h>
#include <string>
#include <utility>


namespace asterx::log {
    inline Common::DriverLog &driver_log() {
        static Common::DriverLog instance;
        return instance;
    }

    // NOT thread-safe: call once before the Qt worker thread exists; read-only afterwards.
    inline void configure(std::string module, spdlog::level::level_enum min_level) {
        driver_log().configure(std::move(module), min_level);
    }

    template<typename... A>
    void trace(spdlog::format_string_t<A...> f, A &&... a) { driver_log().trace(f, std::forward<A>(a)...); }

    template<typename... A>
    void debug(spdlog::format_string_t<A...> f, A &&... a) { driver_log().debug(f, std::forward<A>(a)...); }

    template<typename... A>
    void info(spdlog::format_string_t<A...> f, A &&... a) { driver_log().info(f, std::forward<A>(a)...); }

    template<typename... A>
    void warn(spdlog::format_string_t<A...> f, A &&... a) { driver_log().warn(f, std::forward<A>(a)...); }

    template<typename... A>
    void error(spdlog::format_string_t<A...> f, A &&... a) { driver_log().error(f, std::forward<A>(a)...); }

    template<typename... A>
    void critical(spdlog::format_string_t<A...> f, A &&... a) { driver_log().critical(f, std::forward<A>(a)...); }
} // namespace asterx::log
