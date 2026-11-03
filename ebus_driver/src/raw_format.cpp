#include "ebus_driver/raw_format.h"

#include <cstring>
#if defined(__x86_64__)
#include <nmmintrin.h>
#endif


namespace ebus_driver::raw {
    namespace {
        template<typename T>
        void Put(std::uint8_t *out, T value) {
            for (std::size_t i = 0; i < sizeof(T); ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
        }

        std::uint32_t Software(const std::uint8_t *p, std::size_t size) {
            static const auto table = [] {
                std::array<std::uint32_t, 256> result{};
                for (std::uint32_t i = 0; i < result.size(); ++i) {
                    auto crc = i;
                    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78u : 0);
                    result[i] = crc;
                }
                return result;
            }();
            std::uint32_t crc = ~0u;
            while (size--) crc = table[(crc ^ *p++) & 255] ^ (crc >> 8);
            return ~crc;
        }
#if defined(__x86_64__)
        __attribute__((target("sse4.2"))) std::uint32_t Hardware(const std::uint8_t *p, std::size_t size) {
            std::uint64_t crc = ~0u;
            while (size >= 8) {
                std::uint64_t value;
                std::memcpy(&value, p, sizeof(value));
                crc = _mm_crc32_u64(crc, value);
                p += 8;
                size -= 8;
            }
            auto tail = static_cast<std::uint32_t>(crc);
            while (size--) tail = _mm_crc32_u8(tail, *p++);
            return ~tail;
        }
#endif
    } // namespace

    std::uint32_t Crc32c(const void *data, std::size_t size) {
        const auto *p = static_cast<const std::uint8_t *>(data);
#if defined(__x86_64__)
        if (__builtin_cpu_supports("sse4.2")) return Hardware(p, size);
#endif
        return Software(p, size);
    }

    FileHeader MakeFileHeader(std::uint64_t created, std::uint64_t segment) {
        FileHeader header{};
        std::memcpy(header.data(), "EBUSRAW\0", 8);
        Put(header.data() + 8, kVersion);
        Put(header.data() + 10, static_cast<std::uint16_t>(kFileHeaderSize));
        Put(header.data() + 16, created);
        Put(header.data() + 24, segment);
        Put(header.data() + 60, Crc32c(header.data(), 60));
        return header;
    }

    RecordHeader MakeRecordHeader(std::uint32_t metadata_size, std::uint64_t payload_size,
                                  std::uint64_t sequence, std::uint32_t metadata_crc,
                                  std::uint32_t payload_crc) {
        RecordHeader header{};
        std::memcpy(header.data(), "EBFR", 4);
        Put(header.data() + 4, static_cast<std::uint32_t>(kRecordHeaderSize));
        Put(header.data() + 8, metadata_size);
        Put(header.data() + 16, payload_size);
        Put(header.data() + 24, sequence);
        Put(header.data() + 32, metadata_crc);
        Put(header.data() + 36, payload_crc);
        Put(header.data() + 40, Crc32c(header.data(), 40));
        return header;
    }
} // namespace ebus_driver::raw
