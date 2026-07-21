#ifndef COMMON_UTILITY_H
#define COMMON_UTILITY_H


#include <filesystem>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <yaml-cpp/yaml.h>

// Most driver sources include only "utility.h" for both ConfigLoader and the
// Common::Log API — keep the logging declarations reachable through here.
#include "logger.h"


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


	// Resolve the directory containing the running executable (used for relative config paths)
	std::filesystem::path GetExecutableDir();
}  // namespace Common


#endif	// COMMON_UTILITY_H
