#pragma once

#include <string>
#include <vector>

#include "app_config.hpp"

namespace fx10 {
    // Resolved per-band wavelength axis for the ENVI header.
    struct Wavelengths {
        std::vector<double> nm; // size == bands
        std::vector<double> fwhm; // size == bands, or empty (omitted from the .hdr)
        std::string source_tag; // "file:<path>" | "list" | "generated-linear"
    };

    // Resolve from the configured source:
    //    - file: text file, '#' comments, comma/whitespace separated; either one column
    //            (wavelength) or two columns (wavelength fwhm), consistently.
    //    - list: explicit values from YAML (fwhm from fwhm_list when non-empty).
    //    - grid: linspace grid_start_nm..grid_end_nm inclusive over `bands` points,
    //            tagged "generated-linear" so downstream knows it is approximate.
    // Throws ConfigError when counts do not match `bands` or the file is unreadable.
    Wavelengths resolveWavelengths(const WavelengthConfig &config, int bands);
} // namespace fx10
