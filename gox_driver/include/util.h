#pragma once

#include <cstdint>
#include <cstddef>
#include <string>


namespace gox {
    // Fills 16 bytes with a version-4 UUID (random from /dev/urandom).
    void GenUuidV4(uint8_t out[16]);

    // Lowercase hex of the first n bytes, no separators.
    std::string HexPrefix(const uint8_t *data, size_t n);

    // Publish a complete, durable metadata snapshot without replacing an existing
    // file. Throws std::runtime_error; acquisition must not start on failure.
    void PublishMetadata(const std::string &path, const std::string &text);
} // namespace gox
