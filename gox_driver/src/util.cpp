#include "util.h"

#include <fstream>
#include <random>

#include "time_util.h"


namespace gox {
    void GenUuidV4(uint8_t out[16]) {
        std::ifstream urandom("/dev/urandom", std::ios::binary);
        if (urandom.read(reinterpret_cast<char *>(out), 16) && urandom.gcount() == 16) {
            // ok
        } else {
            // Fallback: seeded PRNG. Only reached on systems without /dev/urandom.
            std::mt19937_64 rng(common::TimeUtil::RealtimeNowNs() ^ common::TimeUtil::MonotonicNowNs());
            for (int i = 0; i < 16; i += 8) {
                uint64_t v = rng();
                for (int j = 0; j < 8; ++j) {
                    out[i + j] = static_cast<uint8_t>(v >> (j * 8));
                }
            }
        }
        out[6] = static_cast<uint8_t>((out[6] & 0x0F) | 0x40); // version 4
        out[8] = static_cast<uint8_t>((out[8] & 0x3F) | 0x80); // variant 10xx
    }

    std::string HexPrefix(const uint8_t *data, size_t n) {
        static const char *digits = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            s.push_back(digits[data[i] >> 4]);
            s.push_back(digits[data[i] & 0x0F]);
        }
        return s;
    }
} // namespace gox
