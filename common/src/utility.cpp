#include "utility.h"
#include "logger.h"

#include <climits>
#include <unistd.h>


namespace common {
    ConfigLoader::ConfigLoader(std::string_view path) {
        try {
            root_ = YAML::LoadFile(std::string{path});
        } catch (const YAML::Exception &e) {
            Log::LogAndThrow("ConfigLoader",
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


    std::filesystem::path GetAbsolutePath(const std::filesystem::path &path) {
        if (path.empty()) {
            Log::LogAndThrow("PathResolver", "Cannot resolve an empty path.");
        }

        std::filesystem::path resolved;
        try {
            resolved = path.is_absolute() ? path : std::filesystem::absolute(path);

            resolved = resolved.lexically_normal();

            if (resolved != resolved.root_path() && resolved.filename().empty()) {
                resolved = resolved.parent_path();
            }
        } catch (const std::filesystem::filesystem_error &e) {
            Log::LogAndThrow("PathResolver", "Cannot resolve absolute path from '" + path.string() + "': " + e.what());
        }
        return resolved;
    }


    // Human-readable byte size, e.g. "1.21 GiB"
    std::string HumanBytes(std::uint64_t bytes) {
        static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
        double v = static_cast<double>(bytes);
        int u = 0;
        while (v >= 1024.0 && u < 4) {
            v /= 1024.0;
            ++u;
        }
        char buf[32];
        if (u == 0) {
            snprintf(buf, sizeof(buf), "%" PRIu64 " B", bytes);
        } else {
            snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
        }
        return buf;
    }
} // namespace common
