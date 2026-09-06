#pragma once

#include <cstdint>
#include <string>


namespace gox {
    // Fills 16 bytes with a version-4 UUID (random from /dev/urandom).
    void GenUuidV4(uint8_t out[16]);

    // Lowercase hex of the first n bytes, no separators.
    std::string HexPrefix(const uint8_t *data, size_t n);
} // namespace gox
