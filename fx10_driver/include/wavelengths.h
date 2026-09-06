#pragma once

#include <string>
#include <vector>

#include "app_config.h"

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
    Wavelengths ResolveWavelengths(const WavelengthConfig &config, int bands);

    // Scientific axes require an operator-supplied calibration profile matching
    // the actual delivered image. source=none preserves uncalibrated DN only.
    Wavelengths ResolveForGeometry(const WavelengthConfig &config, const std::string &serial,
                                  std::int64_t samples, std::int64_t bands,
                                  std::int64_t offset_x, std::int64_t offset_y, bool status_line);
} // namespace fx10
