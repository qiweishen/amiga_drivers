#include "pixel_format.hpp"


namespace fx10 {
    namespace {
        constexpr PixelFormatInfo kFormats[] = {
            {"Mono8", false, 1},
            {"Mono10", false, 2},
            {"Mono10Packed", true, 2},
            {"Mono12", false, 2},
            {"Mono12Packed", true, 2},
        };
    } // namespace


    const PixelFormatInfo *pixelFormatInfo(const std::string &name) {
        for (const auto &info: kFormats) {
            if (name == info.name) {
                return &info;
            }
        }
        return nullptr;
    }


    std::size_t wireBytes(const PixelFormatInfo &info, std::size_t pixels) {
        if (!info.packed) {
            return pixels * info.storage_bpp;
        }
        // 2 pixels per 3 bytes; a trailing odd pixel still occupies a 3-byte
        // group on the wire (defensive — FX10 pixel counts are always even)
        return (pixels / 2) * 3 + (pixels % 2 != 0 ? 3 : 0);
    }


    void unpackMono12Packed(const std::uint8_t *src, std::size_t n_pixels, std::uint16_t *dst) {
        std::size_t s = 0;
        std::size_t d = 0;
        for (; d + 2 <= n_pixels; d += 2, s += 3) {
            const std::uint8_t b0 = src[s];
            const std::uint8_t b1 = src[s + 1];
            const std::uint8_t b2 = src[s + 2];
            dst[d] = static_cast<std::uint16_t>((b0 << 4) | (b1 & 0x0F));
            dst[d + 1] = static_cast<std::uint16_t>((b2 << 4) | (b1 >> 4));
        }
        if (d < n_pixels) { // odd tail (defensive, see wireBytes)
            dst[d] = static_cast<std::uint16_t>((src[s] << 4) | (src[s + 1] & 0x0F));
        }
    }


    void unpackMono10Packed(const std::uint8_t *src, std::size_t n_pixels, std::uint16_t *dst) {
        std::size_t s = 0;
        std::size_t d = 0;
        for (; d + 2 <= n_pixels; d += 2, s += 3) {
            const std::uint8_t b0 = src[s];
            const std::uint8_t b1 = src[s + 1];
            const std::uint8_t b2 = src[s + 2];
            dst[d] = static_cast<std::uint16_t>((b0 << 2) | (b1 & 0x03));
            dst[d + 1] = static_cast<std::uint16_t>((b2 << 2) | ((b1 >> 4) & 0x03));
        }
        if (d < n_pixels) {
            dst[d] = static_cast<std::uint16_t>((src[s] << 2) | (src[s + 1] & 0x03));
        }
    }
} // namespace fx10
