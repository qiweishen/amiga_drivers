#include "drivers_json.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "logger.h"
#include "time_util.h"


namespace Common {
	namespace {
		DriverLog g_log{ "DriversJson" };

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
		}
	} // namespace


	DriversJson::DriversJson(std::string path) : path_(std::move(path)), start_(std::chrono::steady_clock::now()) {
		doc_["run"] = nlohmann::ordered_json{ { "started", TimeUtil::Iso8601UtcSec(NowRealtimeNs()) },
											  { "status", "running" } };
		doc_["drivers"] = nlohmann::ordered_json::object();
	}


	void DriversJson::SetRun(const std::string &timestamp, const std::string &output_directory, bool logging_enabled) {
		doc_["run"]["timestamp"] = timestamp;
		doc_["run"]["output_directory"] = output_directory;
		doc_["run"]["logging_enabled"] = logging_enabled;
	}


	void DriversJson::SetVersion(const std::string &version, const std::string &git_sha) {
		doc_["run"]["version"] = version;
		doc_["run"]["git_sha"] = git_sha;
	}


	void DriversJson::AddDriver(const std::string &name, bool enabled, const std::string &config_path) {
		doc_["drivers"][name] = nlohmann::ordered_json{ { "enabled", enabled }, { "config", config_path } };
	}


	void DriversJson::WriteRunning() {
		doc_["run"]["status"] = "running";
		Write_();
	}


	void DriversJson::Finalize(const std::string &status) {
		doc_["run"]["status"] = status;
		doc_["run"]["ended"] = TimeUtil::Iso8601UtcSec(NowRealtimeNs());
		doc_["run"]["duration_s"] =
				std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
		Write_();
	}


	void DriversJson::Write_() {
		try {
			WriteJsonAtomic(path_, doc_);
		} catch (const std::exception &e) {
			g_log.warn("cannot write {}: {}", path_, e.what());
		}
	}
} // namespace Common
