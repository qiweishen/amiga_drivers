#include "core/session_log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

#include "core/util.hpp"

namespace jai {

	void write_json_atomic(const std::string &path, const nlohmann::ordered_json &doc) {
		std::string tmp = path + ".tmp";
		int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd < 0) {
			throw std::runtime_error("open " + tmp + ": " + std::strerror(errno));
		}
		std::string text = doc.dump(2);
		text.push_back('\n');
		const char *p = text.data();
		size_t left = text.size();
		while (left > 0) {
			ssize_t n = ::write(fd, p, left);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				int saved = errno;
				::close(fd);
				throw std::runtime_error("write " + tmp + ": " + std::strerror(saved));
			}
			p += n;
			left -= static_cast<size_t>(n);
		}
		if (::fdatasync(fd) != 0) {
			int saved = errno;
			::close(fd);
			throw std::runtime_error("fdatasync " + tmp + ": " + std::strerror(saved));
		}
		::close(fd);
		if (::rename(tmp.c_str(), path.c_str()) != 0) {
			throw std::runtime_error("rename " + tmp + " -> " + path + ": " + std::strerror(errno));
		}
	}

	EventLog::~EventLog() {
		close();
	}

	bool EventLog::open(const std::string &path) {
		std::lock_guard<std::mutex> lock(mutex_);
		fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		return fd_ >= 0;
	}

	void EventLog::log(const std::string &type, nlohmann::ordered_json fields, double min_interval_s) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (fd_ < 0) {
			return;
		}
		uint64_t now_mono = now_monotonic_ns();
		RateState &rs = rate_[type];
		if (min_interval_s > 0 && rs.last_write_mono_ns != 0 &&
			now_mono - rs.last_write_mono_ns < static_cast<uint64_t>(min_interval_s * 1e9)) {
			++rs.suppressed;
			return;
		}

		uint64_t now_real = now_realtime_ns();
		nlohmann::ordered_json doc;
		doc["ts"] = iso8601_utc(now_real);
		doc["ts_ns"] = now_real;
		doc["type"] = type;
		if (rs.suppressed > 0) {
			doc["suppressed"] = rs.suppressed;
			rs.suppressed = 0;
		}
		if (fields.is_object()) {
			for (auto it = fields.begin(); it != fields.end(); ++it) {
				doc[it.key()] = *it;
			}
		}
		std::string line = doc.dump();
		line.push_back('\n');
		const char *p = line.data();
		size_t left = line.size();
		while (left > 0) {
			ssize_t n = ::write(fd_, p, left);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				break;	// event log is best-effort
			}
			p += n;
			left -= static_cast<size_t>(n);
		}
		rs.last_write_mono_ns = now_mono;
	}

	void EventLog::close() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}

}  // namespace jai
