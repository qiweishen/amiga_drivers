#pragma once

#include <filesystem>
#include <cinttypes>
#include <string>
#include <string_view>
#include <yaml-cpp/yaml.h>


namespace common {
    class ConfigLoader {
    public:
        // Load and parse a YAML file, throws std::runtime_error on I/O or parse failure
        explicit ConfigLoader(std::string_view path);

        // Access the parsed YAML root node
        const YAML::Node &Root() const { return root_; }
        YAML::Node &Root() { return root_; }

    private:
        YAML::Node root_;
    };


    // Resolve the directory containing the running executable (used for relative config paths)
    std::filesystem::path GetExecutableDir();

    std::filesystem::path GetAbsolutePath(const std::filesystem::path &path);

    std::string HumanBytes(std::uint64_t bytes);
} // namespace common
