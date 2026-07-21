#include "utility.h"
#include "logger.h"

#include <climits>
#include <unistd.h>


namespace Common {
    ConfigLoader::ConfigLoader(std::string_view path) {
        try {
            root_ = YAML::LoadFile(std::string{path});
        } catch (const YAML::Exception &e) {
            Log::log_and_throw("ConfigLoader",
                               "Cannot load YAML config from '" + std::string{path} + "': " + e.what());
        }
    }


    // Resolve the directory containing the running executable (used for relative config paths)
    std::filesystem::path GetExecutableDir() {
        char buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len == -1) {
            throw std::runtime_error("Failed to get executable path");
        }
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path();
    }
} // namespace Common
