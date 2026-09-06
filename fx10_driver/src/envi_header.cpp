#include "envi_header.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>


namespace fx10 {
    namespace {
        // ENVI '{ }' blocks cannot contain braces; replace them in free text.
        std::string SanitizeBraces(const std::string &text) {
            std::string out = text;
            for (char &c: out) {
                if (c == '{') {
                    c = '(';
                }
                if (c == '}') {
                    c = ')';
                }
            }
            return out;
        }


        // " 400.000, 402.691, ...", 8 values per line, no trailing comma.
        void AppendValueBlock(std::ostringstream &os, const char *key, const std::vector<double> &values) {
            std::ostringstream block;
            block << std::setprecision(std::numeric_limits<double>::max_digits10);
            block << key << " = {\n";
            for (std::size_t i = 0; i < values.size(); ++i) {
                block << " " << values[i];
                if (i + 1 < values.size()) {
                    block << ",";
                }
                if ((i + 1) % 8 == 0 && i + 1 < values.size()) {
                    block << "\n";
                }
            }
            block << "\n}\n";
            os << block.str();
        }
    } // namespace


    std::string GenerateEnviHeader(const EnviHeaderInfo &info) {
        if (info.samples == 0 || info.bands == 0) {
            throw std::invalid_argument("ENVI header: samples and bands must be non-zero");
        }
        if (!info.wavelengths_nm.empty() && info.wavelengths_nm.size() != info.bands) {
            throw std::invalid_argument("ENVI header: wavelength count " +
                                std::to_string(info.wavelengths_nm.size()) +
                                " does not match bands " + std::to_string(info.bands));
        }
        if (!info.fwhm_nm.empty() && info.fwhm_nm.size() != info.bands) {
            throw std::invalid_argument("ENVI header: fwhm count " + std::to_string(info.fwhm_nm.size()) +
                                " does not match bands " + std::to_string(info.bands));
        }

        std::ostringstream os;
        os << "ENVI\n";
        os << "description = {\n" << SanitizeBraces(info.description) << "}\n";
        os << "samples = " << info.samples << "\n";
        os << "lines = " << info.lines << "\n";
        os << "bands = " << info.bands << "\n";
        os << "header offset = 0\n";
        os << "file type = ENVI Standard\n";
        os << "data type = " << static_cast<int>(info.data_type) << "\n";
        os << "interleave = bil\n";
        os << "byte order = 0\n";

        if (!info.acquisition_time_iso.empty()) {
            os << "reference acquisition start time = " << info.acquisition_time_iso << "\n";
        }
        if (!info.wavelengths_nm.empty()) {
            os << "wavelength units = Nanometers\n";
            AppendValueBlock(os, "wavelength", info.wavelengths_nm);
        }
        if (!info.fwhm_nm.empty()) {
            AppendValueBlock(os, "fwhm", info.fwhm_nm);
        }

        return os.str();
    }
} // namespace fx10
