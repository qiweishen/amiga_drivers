#include "drivers_json.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

#include "logger.h"
#include "time_util.h"


namespace common {
    namespace {
        DriverLog g_log{"DriversJson"};

        std::uint64_t NowRealtimeNs() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                .count());
        }

        // Atomic replace (temp file + fsync + rename): readers never see a
        // truncated document. Throws std::runtime_error on failure.
        void WriteJsonAtomic(const std::string &path, const nlohmann::ordered_json &doc) {
            const std::string tmp = path + ".tmp";
            const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) {
                throw std::runtime_error("open " + tmp + ": " + std::strerror(errno));
            }
            std::string text = doc.dump(2);
            text.push_back('\n');
            const char *p = text.data();
            std::size_t left = text.size();
            while (left > 0) {
                const ssize_t n = ::write(fd, p, left);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    const int saved = errno;
                    ::close(fd);
                    throw std::runtime_error("write " + tmp + ": " + std::strerror(saved));
                }
                if (n == 0) {
                    ::close(fd);
                    throw std::runtime_error("write " + tmp + ": no progress");
                }
                p += n;
                left -= static_cast<std::size_t>(n);
            }
            if (::fdatasync(fd) != 0) {
                const int saved = errno;
                ::close(fd);
                throw std::runtime_error("fdatasync " + tmp + ": " + std::strerror(saved));
            }
            ::close(fd);
            if (::rename(tmp.c_str(), path.c_str()) != 0) {
                throw std::runtime_error("rename " + tmp + " -> " + path + ": " + std::strerror(errno));
            }
            auto parent = std::filesystem::path(path).parent_path();
            if (parent.empty()) parent = ".";
            const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dir_fd < 0) throw std::runtime_error("cannot open manifest directory for sync");
            const bool synced = ::fsync(dir_fd) == 0;
            ::close(dir_fd);
            if (!synced) throw std::runtime_error("cannot sync manifest directory");
        }
    } // namespace


    DriversJson::DriversJson(std::string path) : path_(std::move(path)), start_(std::chrono::steady_clock::now()) {
        doc_["run"] = nlohmann::ordered_json{
            {"started", TimeUtil::Iso8601UtcSec(NowRealtimeNs())},
            {"status", "running"}
        };
        doc_["drivers"] = nlohmann::ordered_json::object();
        doc_["time_policy"] = {
            {"reference", "unverified; see enabled driver configs and device readbacks"},
            {"association", "not established by this manifest; requires per-sample identity and a validated clock mapping"},
            {"association_verified", false},
            {"fallback", "none"},
            {"other_clock_fields", "retain native values; units, epoch and lock state must be verified before association"},
            {"host_run_fields", "operational metadata only; not acquisition timestamps"}
        };
    }


    void DriversJson::SetMeta(const std::string &operator_name, const std::string &field_name) {
        doc_["meta"]["operator"] = operator_name;
        doc_["meta"]["field"] = field_name;
    }


    void DriversJson::SetRun(const std::string &timestamp, const std::string &output_directory) {
        doc_["run"]["timestamp"] = timestamp;
        doc_["run"]["output_directory"] = output_directory;
    }


    void DriversJson::SetVersion(const std::string &version, const std::string &git_sha) {
        doc_["run"]["version"] = version;
        doc_["run"]["git_sha"] = git_sha;
    }


    void DriversJson::AddDriver(const std::string &name, bool enabled, const std::string &config_path) {
        doc_["drivers"][name] = nlohmann::ordered_json{{"enabled", enabled}, {"config", config_path}};
    }


    void DriversJson::AddDriverResult(const std::string &name, bool failed, const nlohmann::ordered_json &statistics) {
        doc_["driver_results"].push_back({{"name", name}, {"failed", failed}, {"statistics", statistics}});
    }

    bool DriversJson::WriteRunning() {
        doc_["run"]["status"] = "running";
        return Write_();
    }


    bool DriversJson::Finalize(const std::string &status) {
        doc_["run"]["status"] = status;
        doc_["run"]["recording_failed"] = status.rfind("failed", 0) == 0;
        doc_["run"]["ended"] = TimeUtil::Iso8601UtcSec(NowRealtimeNs());
        doc_["run"]["duration_s"] =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        return Write_();
    }


    bool DriversJson::Write_() {
        try {
            WriteJsonAtomic(path_, doc_);
            return true;
        } catch (const std::exception &e) {
            g_log.Warn("cannot write {}: {}", path_, e.what());
            return false;
        }
    }
} // namespace common
