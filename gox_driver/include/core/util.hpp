#pragma once

#include <cstdint>
#include <string>

namespace jai {

	// Wall clock (CLOCK_REALTIME), nanoseconds since Unix epoch (UTC).
	uint64_t now_realtime_ns();

	// Monotonic clock (CLOCK_MONOTONIC), nanoseconds since an arbitrary origin.
	uint64_t now_monotonic_ns();

	// "2026-07-17T11:54:00.123Z" from a CLOCK_REALTIME timestamp.
	std::string iso8601_utc(uint64_t realtime_ns);

	// Compact UTC stamp usable in file names: "20260717T115400Z".
	std::string compact_utc(uint64_t realtime_ns);

	// Fills 16 bytes with a version-4 UUID (random from /dev/urandom).
	void gen_uuid_v4(uint8_t out[16]);

	// Canonical 8-4-4-4-12 lowercase hex representation.
	std::string uuid_to_string(const uint8_t uuid[16]);

	// Lowercase hex of the first n bytes, no separators.
	std::string hex_prefix(const uint8_t *data, size_t n);

	// "01:23:45:ab:cd:ef" / "01-23-45-AB-CD-EF" / "012345abcdef" -> "012345abcdef".
	// Returns empty string if the input is not a recognizable MAC address.
	std::string normalize_mac(const std::string &mac);

	// Human-readable byte size, e.g. "1.21 GiB".
	std::string human_bytes(uint64_t bytes);

	// "HH:MM:SS" from a duration in seconds.
	std::string human_duration(uint64_t seconds);

}  // namespace jai
