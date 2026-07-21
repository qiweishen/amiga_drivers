#pragma once

#include <sstream>
#include <string>

namespace jai {

	enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

	// Parses "trace|debug|info|warn|error" (case-insensitive). Returns Info on
	// unknown input and sets *ok to false if provided.
	LogLevel parse_log_level(const std::string &s, bool *ok = nullptr);

	// Thin level facade over the common logging layer: every message lands on
	// the process-wide spdlog default logger as "[GoX]: <msg>" (tools without
	// Logger::init fall back to spdlog's implicit stdout logger). Never throws
	// at any level. Use through the LOG_* macros below or logger().
	class Logger {
	public:
		Logger() = default;

		void set_level(LogLevel level);
		LogLevel level() const { return level_; }
		bool enabled(LogLevel level) const { return static_cast<int>(level) >= static_cast<int>(level_); }

		void log(LogLevel level, const std::string &msg);

	private:
		LogLevel level_ = LogLevel::Info;  // cached for a cheap enabled() fast path
	};

	Logger &logger();

	namespace detail {
		inline void log_concat(std::ostringstream &) {}
		template<typename T, typename... Rest>
		void log_concat(std::ostringstream &os, T &&v, Rest &&...rest) {
			os << std::forward<T>(v);
			log_concat(os, std::forward<Rest>(rest)...);
		}
	}  // namespace detail

	template<typename... Args>
	void log_fmt(LogLevel level, Args &&...args) {
		if (!logger().enabled(level)) {
			return;
		}
		std::ostringstream os;
		detail::log_concat(os, std::forward<Args>(args)...);
		logger().log(level, os.str());
	}

}  // namespace jai

#define LOG_TRACE(...) ::jai::log_fmt(::jai::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) ::jai::log_fmt(::jai::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) ::jai::log_fmt(::jai::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) ::jai::log_fmt(::jai::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::jai::log_fmt(::jai::LogLevel::Error, __VA_ARGS__)
