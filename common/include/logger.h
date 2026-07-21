/// @file logger.h
/// @brief The Common logging API: Logger::init (process-wide spdlog setup),
/// Log:: free functions (module-tagged logging, throwing error helper, pre-log
/// callback) and DriverLog (per-driver facade with fmt-style call sites).

#ifndef COMMON_LOGGER_H
#define COMMON_LOGGER_H

#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <utility>


namespace Common {
	namespace Logger {
		struct Config {
			std::string log_file;  // Empty = no file logging
			bool quiet = false;	   // Suppress below-warn messages from the console
		};

		// Initialize the logging system. Call once at startup before any log calls
		// Sets up a dual-sink logger: stderr (INFO+ or WARN+ if quiet) and optional file (TRACE+)
		void init(const Config &config, const std::string &logger_name = "AmigaDriver");
	}  // namespace Logger


	namespace Log {
		// Log at the given spdlog level. NEVER throws (any level) — paths that
		// must abort use log_and_throw(..., throw_error=true) explicitly.
		void log_message(spdlog::level::level_enum level, std::string_view module, std::string_view msg,
						 std::string_view error_detail = "");

		// Log at error level; throws std::runtime_error unless throw_error=false.
		// The ONLY throwing path of the logging API — reserve it for init/config
		// code where aborting is intended, never in worker threads or destructors.
		void log_and_throw(std::string_view module, std::string_view msg, std::string_view error_detail = "", bool throw_error = true);

		// Pre-log callback: invoked before each console log write
		using PreLogCallback = void (*)();
		void set_pre_log_callback(PreLogCallback cb);

		// Invoke the registered pre-log callback (no-op when none registered)
		void run_pre_log_callback();
	}  // namespace Log


	// Per-driver logging facade over Common::Log for driver-core code: fmt-style
	// call sites, a driver-local min-level filter, and NO throwing path at any
	// level, so it is safe inside Qt slots, worker threads and destructors.
	// Output lands on the process-wide default logger as "[<module>]: <msg>"
	// with the original level token, and the pre-log callback (ActivitySpinner
	// line clear) runs inside Log:: automatically.
	class DriverLog {
	public:
		DriverLog() = default;

		explicit DriverLog(std::string module, spdlog::level::level_enum min_level = spdlog::level::trace)
			: module_(std::move(module)), min_level_(min_level) {}

		// NOT thread-safe: call once before the driver's threads exist
		void configure(std::string module, spdlog::level::level_enum min_level) {
			module_ = std::move(module);
			min_level_ = min_level;
		}

		template<typename... Args>
		void log(spdlog::level::level_enum lvl, spdlog::format_string_t<Args...> f, Args &&...args) {
			if (lvl < min_level_) {
				return;
			}
			Log::log_message(lvl, module_, fmt::format(f, std::forward<Args>(args)...));
		}

		template<typename... A>
		void trace(spdlog::format_string_t<A...> f, A &&...a) { log(spdlog::level::trace, f, std::forward<A>(a)...); }
		template<typename... A>
		void debug(spdlog::format_string_t<A...> f, A &&...a) { log(spdlog::level::debug, f, std::forward<A>(a)...); }
		template<typename... A>
		void info(spdlog::format_string_t<A...> f, A &&...a) { log(spdlog::level::info, f, std::forward<A>(a)...); }
		template<typename... A>
		void warn(spdlog::format_string_t<A...> f, A &&...a) { log(spdlog::level::warn, f, std::forward<A>(a)...); }
		template<typename... A>
		void error(spdlog::format_string_t<A...> f, A &&...a) { log(spdlog::level::err, f, std::forward<A>(a)...); }
		template<typename... A>
		void critical(spdlog::format_string_t<A...> f, A &&...a) { log(spdlog::level::critical, f, std::forward<A>(a)...); }

	private:
		std::string module_{ "Driver" };
		spdlog::level::level_enum min_level_{ spdlog::level::trace };
	};
} // namespace Common

#endif // COMMON_LOGGER_H
