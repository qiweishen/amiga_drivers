#include "core/logger.hpp"

#include <algorithm>
#include <cctype>

#include <spdlog/spdlog.h>

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

	void Logger::configure(const std::string &message_prefix, std::function<void()> pre_log_hook) {
		prefix_ = message_prefix;
		pre_log_ = std::move(pre_log_hook);
	}

	void Logger::log(LogLevel level, const std::string &msg) {
		if (!enabled(level)) {
			return;
		}
		if (pre_log_) {
			pre_log_();
		}
		// Emit through the process-wide spdlog default logger (level filtering
		// already happened in enabled(); the message is logged verbatim)
		if (prefix_.empty()) {
			spdlog::default_logger()->log(to_spdlog(level), msg);
		} else {
			spdlog::default_logger()->log(to_spdlog(level), prefix_ + msg);
		}
	}

	Logger &logger() {
		static Logger instance;
		return instance;
	}

}  // namespace jai
