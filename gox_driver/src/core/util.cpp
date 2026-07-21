#include "core/util.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <random>

#include "time_util.h"

namespace jai {

	// Time and human-readable formatting delegate to the shared helpers;
	// the jai:: signatures stay for the existing call sites.
	uint64_t now_realtime_ns() {
		return Common::TimeUtil::RealtimeNowNs();
	}

	uint64_t now_monotonic_ns() {
		return Common::TimeUtil::MonotonicNowNs();
	}

	std::string iso8601_utc(uint64_t realtime_ns) {
		return Common::TimeUtil::Iso8601Utc(realtime_ns);
	}

	std::string compact_utc(uint64_t realtime_ns) {
		return Common::TimeUtil::CompactUtc(realtime_ns);
	}

	std::string human_bytes(uint64_t bytes) {
		return Common::TimeUtil::HumanBytes(bytes);
	}

	std::string human_duration(uint64_t seconds) {
		return Common::TimeUtil::HumanDuration(seconds);
	}

	void gen_uuid_v4(uint8_t out[16]) {
		std::ifstream urandom("/dev/urandom", std::ios::binary);
		if (urandom.read(reinterpret_cast<char *>(out), 16) && urandom.gcount() == 16) {
			// ok
		} else {
			// Fallback: seeded PRNG. Only reached on systems without /dev/urandom.
			std::mt19937_64 rng(now_realtime_ns() ^ now_monotonic_ns());
			for (int i = 0; i < 16; i += 8) {
				uint64_t v = rng();
				for (int j = 0; j < 8; ++j) {
					out[i + j] = static_cast<uint8_t>(v >> (j * 8));
				}
			}
		}
		out[6] = static_cast<uint8_t>((out[6] & 0x0F) | 0x40);	// version 4
		out[8] = static_cast<uint8_t>((out[8] & 0x3F) | 0x80);	// variant 10xx
	}

	std::string uuid_to_string(const uint8_t uuid[16]) {
		char buf[37];
		snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", uuid[0], uuid[1], uuid[2],
				 uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14],
				 uuid[15]);
		return buf;
	}

	std::string hex_prefix(const uint8_t *data, size_t n) {
		static const char *digits = "0123456789abcdef";
		std::string s;
		s.reserve(n * 2);
		for (size_t i = 0; i < n; ++i) {
			s.push_back(digits[data[i] >> 4]);
			s.push_back(digits[data[i] & 0x0F]);
		}
		return s;
	}

	std::string normalize_mac(const std::string &mac) {
		std::string out;
		out.reserve(12);
		for (char c: mac) {
			if (c == ':' || c == '-' || c == '.') {
				continue;
			}
			if (!std::isxdigit(static_cast<unsigned char>(c))) {
				return {};
			}
			out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		return out.size() == 12 ? out : std::string{};
	}

}  // namespace jai
