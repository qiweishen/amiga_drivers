#pragma once

// Supported GigE Vision mono pixel formats and the unpack step that turns the
// wire representation into the canonical storage layout (right-justified
// little-endian uint16, or uint8 for Mono8). The ENVI on-disk contract only
// ever sees the canonical layout — packed formats save link bandwidth, never
// disk bytes.
//
// Both packed formats are the GVSP legacy packing: 2 pixels in 3 bytes
// (1.5 B/px on the wire; the PFNC "Mono10p" 1.25 B/px flavor is a different
// format and is not offered by the FX10e).

#include <cstddef>
#include <cstdint>
#include <string>


namespace fx10 {
    struct PixelFormatInfo {
        const char *name;
        bool packed; // GVSP 2-pixels-in-3-bytes wire packing
        std::uint32_t storage_bpp; // bytes per pixel after unpack (1 or 2)
    };

    // nullptr = format not supported by this driver
    [[nodiscard]] const PixelFormatInfo *pixelFormatInfo(const std::string &name);

    // Wire bytes for `pixels` pixels (pixels must be even for packed formats;
    // FX10 frames are 1024 samples wide so totals are always even)
    [[nodiscard]] std::size_t wireBytes(const PixelFormatInfo &info, std::size_t pixels);

    // GVSP Mono12Packed: b0 = P0[11:4], b1 = (P1[3:0] << 4) | P0[3:0], b2 = P1[11:4]
    void unpackMono12Packed(const std::uint8_t *src, std::size_t n_pixels, std::uint16_t *dst);

    // GVSP Mono10Packed: b0 = P0[9:2], b1 bits[1:0] = P0[1:0], bits[5:4] = P1[1:0], b2 = P1[9:2]
    void unpackMono10Packed(const std::uint8_t *src, std::size_t n_pixels, std::uint16_t *dst);
} // namespace fx10
