#pragma once

// Session-level outputs shared across cameras:
//   session.json  written at start (status=recording) and atomically
//                 replaced at clean shutdown (status=completed)
//   events.jsonl  low-frequency append-only event log

#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace jai {

	// Writes JSON to path atomically (temp file + fsync + rename). Throws
	// std::runtime_error on failure.
	void write_json_atomic(const std::string &path, const nlohmann::ordered_json &doc);

	// Append-only JSONL event log with per-type rate limiting. Thread-safe.
	class EventLog {
	public:
		EventLog() = default;
		~EventLog();

		// Opens (creates/appends) the log file. Returns false on failure.
		bool open(const std::string &path);

		// Appends {"ts": <iso8601>, "ts_ns": ..., "type": type, ...fields}.
		// When min_interval_s > 0, events of the same type within that window
		// are counted but not written; the next written event of that type
		// carries a "suppressed" count.
		void log(const std::string &type, nlohmann::ordered_json fields = {}, double min_interval_s = 0.0);

		void close();

	private:
		std::mutex mutex_;
		int fd_ = -1;
		struct RateState {
			uint64_t last_write_mono_ns = 0;
			uint64_t suppressed = 0;
		};
		std::unordered_map<std::string, RateState> rate_;
	};

}  // namespace jai
