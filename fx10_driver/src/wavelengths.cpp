#include "../include/wavelengths.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace fx10 {
    namespace {
        Wavelengths FromFile(const std::string &path, int bands) {
            std::ifstream file(path);
            if (!file) {
                throw ConfigError("wavelength file '" + path + "': cannot open");
            }

            Wavelengths out;
            out.source_tag = "file:" + path;
            int columns = 0; // 0 = undecided; then 1 or 2, must stay consistent
            std::string line;
            int line_no = 0;
            while (std::getline(file, line)) {
                ++line_no;
                const std::size_t hash = line.find('#');
                if (hash != std::string::npos) line.erase(hash);
                for (char &c: line) {
                    if (c == ',') {
                        c = ' ';
                    }
                }
                std::istringstream tokens(line);
                double first = 0.0;
                if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
                if (!(tokens >> first)) {
                    throw ConfigError("wavelength file '" + path + "' line " + std::to_string(line_no) +
                                      ": invalid numeric value");
                }
                double second = 0.0;
                const bool has_second = static_cast<bool>(tokens >> second);
                if (!has_second) tokens.clear(); // failbit would mask trailing-garbage detection
                std::string extra;
                if (tokens >> extra) {
                    throw ConfigError("wavelength file '" + path + "' line " + std::to_string(line_no) +
                                      ": expected 1 or 2 numbers per line");
                }
                const int line_columns = has_second ? 2 : 1;
                if (columns == 0) {
                    columns = line_columns;
                }
                if (line_columns != columns) {
                    throw ConfigError("wavelength file '" + path + "' line " + std::to_string(line_no) +
                                      ": inconsistent column count (file mixes 1- and 2-column lines)");
                }
                out.nm.push_back(first);
                if (out.nm.size() > static_cast<std::size_t>(bands)) {
                    throw ConfigError("wavelength file '" + path + "': too many band entries");
                }
                if (has_second) {
                    out.fwhm.push_back(second);
                }
            }
            if (file.bad()) {
                throw ConfigError("wavelength file '" + path + "': read failed");
            }
            if (out.nm.empty()) {
                throw ConfigError("wavelength file '" + path + "': no numeric values found");
            }
            if (static_cast<int>(out.nm.size()) != bands) {
                throw ConfigError("wavelength file '" + path + "': " + std::to_string(out.nm.size()) +
                                  " values but the configuration expects " + std::to_string(bands) +
                                  " bands");
            }
            return out;
        }
    } // namespace


    Wavelengths ResolveWavelengths(const WavelengthConfig &config, int bands) {
        if (bands <= 0) {
            throw ConfigError("Wavelengths: band count must be positive, got " + std::to_string(bands));
        }

        Wavelengths out;
        switch (config.source) {
            case WavelengthSource::kNone:
                if (!config.fwhm_list.empty()) {
                    throw ConfigError("wavelength source=none cannot carry a spectral FWHM axis");
                }
                out.source_tag = "none (UNCALIBRATED; image rows are not assigned wavelengths)";
                return out;
            case WavelengthSource::kFile:
                out = FromFile(config.file, bands);
                break;
            case WavelengthSource::kList:
                if (static_cast<int>(config.list.size()) != bands) {
                    throw ConfigError("recording.wavelengths.list has " + std::to_string(config.list.size()) +
                                      " values but the configuration expects " + std::to_string(bands) +
                                      " bands");
                }
                out.nm = config.list;
                out.source_tag = "list";
                break;
            case WavelengthSource::kGrid: {
                out.nm.reserve(static_cast<std::size_t>(bands));
                if (bands == 1) {
                    out.nm.push_back(config.grid_start_nm);
                } else {
                    const double step = (config.grid_end_nm - config.grid_start_nm) / (bands - 1);
                    for (int i = 0; i < bands; ++i) {
                        out.nm.push_back(config.grid_start_nm + step * i);
                    }
                    out.nm.back() = config.grid_end_nm; // exact endpoint, no accumulation error
                }
                out.source_tag = "generated-linear";
                break;
            }
        }

        // Explicit fwhm_list overrides/supplements any source (including 2-column files).
        if (!config.fwhm_list.empty()) {
            if (static_cast<int>(config.fwhm_list.size()) != bands) {
                throw ConfigError("recording.wavelengths.fwhm_list has " +
                                  std::to_string(config.fwhm_list.size()) +
                                  " values but the configuration expects " + std::to_string(bands) +
                                  " bands");
            }
            out.fwhm = config.fwhm_list;
        }
        if (!out.fwhm.empty() && out.fwhm.size() != out.nm.size()) {
            throw ConfigError("Wavelengths: fwhm count " + std::to_string(out.fwhm.size()) +
                              " does not match wavelength count " + std::to_string(out.nm.size()));
        }

        // Guard the .hdr against garbage: values must be finite and physically sane
        // (the FX10 family spans 400-1000 nm; anything outside 1-100000 nm is a
        // malformed calibration file or config, not a real camera).
        for (const double v: out.nm) {
            if (!std::isfinite(v) || v < 1.0 || v > 100000.0) {
                throw ConfigError("Wavelengths: non-finite or out-of-range value " + std::to_string(v));
            }
        }
        for (const double v: out.fwhm) {
            if (!std::isfinite(v) || v < 0.0 || v > 10000.0) {
                throw ConfigError("Wavelengths: non-finite or out-of-range fwhm value " + std::to_string(v));
            }
        }
        return out;
    }

    Wavelengths ResolveForGeometry(const WavelengthConfig &config, const std::string &serial,
                                  std::int64_t samples, std::int64_t bands,
                                  std::int64_t offset_x, std::int64_t offset_y, bool status_line) {
        if (samples <= 0 || samples > 65535 || bands <= 0 || bands > 65535) {
            throw ConfigError("invalid delivered image geometry");
        }
        if (config.source == WavelengthSource::kNone) {
            return ResolveWavelengths(config, static_cast<int>(bands));
        }
        if (config.source == WavelengthSource::kGrid) {
            throw ConfigError("generated wavelength grids are not calibration: use source=none for "
                              "uncalibrated DN, or file/list with a matching calibration profile");
        }
        const auto &cal = config.calibration;
        if (cal.reference.empty() || cal.device_serial.empty() || cal.samples <= 0 || cal.bands <= 0 ||
            cal.offset_x < 0 || cal.offset_y < 0) {
            throw ConfigError("wavelength file/list requires calibration reference, device_serial, "
                              "samples, bands, offset_x and offset_y from the calibration pack");
        }
        if (serial != cal.device_serial || samples != cal.samples || bands != cal.bands ||
            offset_x != cal.offset_x || offset_y != cal.offset_y || status_line) {
            throw ConfigError("delivered image does not match the spectral calibration profile "
                              "(serial/geometry/offset/status-line); refusing to assign wavelengths");
        }
        auto out = ResolveWavelengths(config, static_cast<int>(bands));
        out.source_tag += "; operator calibration reference: " + cal.reference;
        return out;
    }
} // namespace fx10
