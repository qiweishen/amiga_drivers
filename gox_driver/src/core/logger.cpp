#include "core/logger.hpp"

#include <algorithm>
#include <cctype>

#include <spdlog/spdlog.h>

#include "logger.h"

namespace jai {

	LogLevel parse_log_level(const std::string &s, bool *ok) {
		std::string lower = s;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (ok) {
			*ok = true;
		}
		if (lower == "trace")
			return LogLevel::Trace;
		if (lower == "debug")
			return LogLevel::Debug;
		if (lower == "info")
			return LogLevel::Info;
		if (lower == "warn" || lower == "warning")
			return LogLevel::Warn;
		if (lower == "error")
			return LogLevel::Error;
		if (ok) {
			*ok = false;
		}
		return LogLevel::Info;
	}

	namespace {

		spdlog::level::level_enum to_spdlog(LogLevel level) {
			switch (level) {
				case LogLevel::Trace:
					return spdlog::level::trace;
				case LogLevel::Debug:
					return spdlog::level::debug;
				case LogLevel::Info:
					return spdlog::level::info;
				case LogLevel::Warn:
					return spdlog::level::warn;
				case LogLevel::Error:
					return spdlog::level::err;
			}
			return spdlog::level::info;
		}

	}  // namespace

	void Logger::set_level(LogLevel level) {
		level_ = level;
	}

	void Logger::log(LogLevel level, const std::string &msg) {
		if (!enabled(level)) {
			return;
		}
		// Emit through the common logging layer: "[GoX]: <msg>" on the
		// process-wide spdlog default logger; the pre-log callback (spinner
		// line clear) runs inside Common::Log. The "GoX" module token is
		// distinct from "GoXApp" on purpose — only App-level lines drive the
		// GUI health state machine.
		Common::Log::log_message(to_spdlog(level), "GoX", msg);
	}

	Logger &logger() {
		static Logger instance;
		return instance;
	}

}  // namespace jai
