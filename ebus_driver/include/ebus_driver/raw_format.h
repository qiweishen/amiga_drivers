#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ebus_driver::raw {
    inline constexpr std::size_t kFileHeaderSize = 64;
    inline constexpr std::size_t kRecordHeaderSize = 48;
    inline constexpr std::uint32_t kMaxMetadataSize = 1024 * 1024;
    inline constexpr std::uint16_t kVersion = 1;
    // All integers are explicitly serialized little-endian; no C++ object ABI is stored.
    using FileHeader = std::array<std::uint8_t, kFileHeaderSize>;
    using RecordHeader = std::array<std::uint8_t, kRecordHeaderSize>;

    std::uint32_t Crc32c(const void *data, std::size_t size);

    FileHeader MakeFileHeader(std::uint64_t created_realtime_ns, std::uint64_t segment_index);

    RecordHeader MakeRecordHeader(std::uint32_t metadata_size, std::uint64_t payload_size,
                                  std::uint64_t sequence, std::uint32_t metadata_crc,
                                  std::uint32_t payload_crc);
} // namespace ebus_driver::raw
