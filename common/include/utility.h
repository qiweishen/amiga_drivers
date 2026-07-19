#ifndef COMMON_UTILITY_H
#define COMMON_UTILITY_H


#include <filesystem>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <yaml-cpp/yaml.h>


namespace Common {
	class ConfigLoader {
	public:
		// Load and parse a YAML file, throws std::runtime_error on I/O or parse failure
		explicit ConfigLoader(std::string_view path);

		// Access the parsed YAML root node
		[[nodiscard]] const YAML::Node &root() const { return root_; }
		[[nodiscard]] YAML::Node &root() { return root_; }

		// Convenience accessor: root()[section][key].as<T>(default_val)
		template<typename T>
		T get(std::string_view section, std::string_view key, const T &default_val) const {
			return root_[std::string{ section }][std::string{ key }].template as<T>(default_val);
		}

	private:
		YAML::Node root_;
	};


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
		// Log at the given spdlog level
		void log_message(spdlog::level::level_enum level, std::string_view module, std::string_view msg,
						 std::string_view error_detail = "");

		// Log at error level and throw std::runtime_error
		// Use for unrecoverable errors where exception-based unwinding is needed
		void log_and_throw(std::string_view module, std::string_view msg, std::string_view error_detail = "", bool throw_error = true);

		// Pre-log callback: invoked before each console log write
		using PreLogCallback = void (*)();
		void set_pre_log_callback(PreLogCallback cb);

		// Invoke the registered pre-log callback (no-op when none registered).
		// For logging backends that write via spdlog directly instead of
		// log_message() (e.g. the GoX driver's jai::Logger bridge)
		void run_pre_log_callback();
	}  // namespace Log


	// Resolve the directory containing the running executable (used for relative config paths)
	std::filesystem::path GetExecutableDir();
}  // namespace Common


#endif	// COMMON_UTILITY_H
