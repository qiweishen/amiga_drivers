#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

// Fixed-width binary `.times` sidecar: one 48-byte record per recorded line, after
// a 64-byte file header. Record k lives at byte 64 + k*48 — O(1) random access
// mirroring the BIL file's fixed line stride. Byte order is the host's
// little-endian (x86_64 / aarch64); documented in README with a NumPy dtype.

namespace fx10 {
    inline constexpr char kSidecarMagic[8] = {'F', 'X', '1', '0', 'T', 'S', '0', '1'};
    inline constexpr std::uint32_t kSidecarVersion = 1;

    struct SidecarHeader {
        char magic[8]; // "FX10TS01"
        std::uint32_t version; // kSidecarVersion
        std::uint32_t record_size; // sizeof(SidecarRecord) == 48
        std::uint64_t tick_frequency_hz; // GevTimestampTickFrequency; 0 = unknown
        std::uint64_t session_start_realtime_ns; // CLOCK_REALTIME at session start
        std::uint64_t session_start_monotonic_ns; // CLOCK_MONOTONIC, same instant
        std::uint32_t segment_index; // 1-based, matches segment_%04u files
        std::uint8_t reserved[20]; // zero
    };

    static_assert(sizeof(SidecarHeader) == 64, "sidecar header must be exactly 64 bytes");
    static_assert(std::is_trivially_copyable_v<SidecarHeader>);

    // SidecarRecord.flags bits:
    inline constexpr std::uint32_t kFlagGapBefore = 1u << 0; // frames missing before this line
    inline constexpr std::uint32_t kFlagDeviceTsValid = 1u << 1; // device_timestamp_ticks meaningful
    inline constexpr std::uint32_t kFlagPaddedZero = 1u << 2; // synthetic zero line (pad_zero policy)

    struct SidecarRecord {
        std::uint64_t block_id; // GVSP BlockID; 0 for padded lines
        std::uint64_t host_realtime_ns; // CLOCK_REALTIME at retrieve
        std::uint64_t host_monotonic_ns; // CLOCK_MONOTONIC, same instant
        std::uint64_t device_timestamp_ticks; // GVSP timestamp; valid iff kFlagDeviceTsValid
        std::uint64_t global_line_index; // 0-based, spans segments
        std::uint32_t flags;
        // Frames missed immediately before this record. Invariant: the sum of gap_len
        // over all records of a session equals the total frames missed on RX (a padded
        // run splits one gap between the first padded record and, when the pad cap was
        // hit, the next real record — never double-counted).
        std::uint32_t gap_len;
    };

    static_assert(sizeof(SidecarRecord) == 48, "sidecar record must be exactly 48 bytes");
    static_assert(std::is_trivially_copyable_v<SidecarRecord>);

    // Header factory with magic/version/record_size prefilled and reserved zeroed.
    SidecarHeader makeSidecarHeader(std::uint64_t tick_frequency_hz,
                                    std::uint64_t session_start_realtime_ns,
                                    std::uint64_t session_start_monotonic_ns,
                                    std::uint32_t segment_index);

    // True when `header` carries the expected magic, version, and record size.
    bool sidecarHeaderValid(const SidecarHeader &header);
} // namespace fx10
