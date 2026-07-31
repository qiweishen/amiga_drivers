#pragma once

#include <cstdint>
#include <string>
#include <vector>


// Pure ENVI .hdr text generation. No file I/O here
// The recorder decides where and when the header lands (it is written LAST: `.hdr` present <=> segment valid).

namespace fx10 {
    enum class EnviDataType : int {
        kUint8 = 1, // Mono8
        kUint16 = 12 // Mono10/Mono12 delivered as 2-byte little-endian
    };

    struct EnviHeaderInfo {
        std::uint32_t samples = 0; // spatial pixels per line (1024 full-width)
        std::uint64_t lines = 0; // frames written (known only at finalize)
        std::uint32_t bands = 0;
        EnviDataType data_type = EnviDataType::kUint16;
        std::string description; // free text; braces are sanitized
        std::string acquisition_time_iso; // ISO 8601 UTC; empty = omit
        std::vector<double> wavelengths_nm; // size must equal bands when non-empty
        std::vector<double> fwhm_nm; // size must equal bands when non-empty
    };

    // Render the header text. Throws std::invalid_argument on zero geometry or a
    // wavelength/fwhm count that does not match `bands`.
    std::string generateEnviHeader(const EnviHeaderInfo &info);
} // namespace fx10
