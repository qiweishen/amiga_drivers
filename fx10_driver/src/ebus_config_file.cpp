#include "ebus_config_file.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

namespace fx10 {
    namespace {
        std::string Trim(const std::string &s) {
            const auto not_space = [](unsigned char c) { return !std::isspace(c); };
            const auto begin = std::find_if(s.begin(), s.end(), not_space);
            const auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
            return begin < end ? std::string(begin, end) : std::string();
        }

        std::string ReadBytes(const std::filesystem::path &path, const char *what) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                throw ConfigError(std::string(what) + " '" + path.string() + "': cannot open");
            }
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (in.bad()) {
                throw ConfigError(std::string(what) + " '" + path.string() + "': read error");
            }
            return bytes;
        }

        std::string Lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }
    } // namespace


    std::string Sha256File(const std::filesystem::path &path) {
        const std::string bytes = ReadBytes(path, "device.ebus_config");
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) != 1) {
            throw ConfigError("device.ebus_config '" + path.string() + "': OpenSSL SHA-256 failed");
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string hex;
        for (unsigned int i = 0; i < length; ++i) {
            hex.push_back(kHex[digest[i] >> 4]);
            hex.push_back(kHex[digest[i] & 0xF]);
        }
        return hex;
    }


    void CheckEbusConfigSerial(const std::filesystem::path &pvcfg, const std::string &device_serial) {
        const std::string expected = pvcfg.parent_path().filename().string();
        const std::string actual = Trim(device_serial);
        if (expected.empty() || actual != expected) {
            throw ConfigError("device.ebus_config '" + pvcfg.string() + "' belongs to serial '" + expected +
                              "', but the connected camera reports DeviceSerialNumber '" + actual + "'");
        }
    }


    std::string CheckEbusConfigChecksum(const std::filesystem::path &pvcfg) {
        // sha256sum format: "<hash> <' ' or '*'><file name>"
        const auto checksum_file = pvcfg.parent_path() / "checksums.txt";
        std::istringstream lines(ReadBytes(checksum_file, "ebus_config checksum file"));
        const std::string name = pvcfg.filename().string();
        std::string line, expected;
        int matches = 0;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string hash, file;
            if (!(fields >> hash >> file)) continue; // blank line
            if (!file.empty() && file.front() == '*') file.erase(0, 1);
            if (file == name) {
                expected = Lower(hash);
                ++matches;
            }
        }
        if (matches != 1) {
            throw ConfigError(checksum_file.string() + ": expected exactly one entry for '" + name + "', found " +
                              std::to_string(matches));
        }
        const std::string actual = Sha256File(pvcfg);
        if (actual != expected) {
            throw ConfigError("device.ebus_config '" + pvcfg.string() + "': SHA-256 " + actual +
                              " does not match " + checksum_file.string() + " (" + expected + ")");
        }
        return actual;
    }


    std::string FormatSensorGeometry(const SensorGeometry &g) {
        return "Width=" + std::to_string(g.width) + " Height=" + std::to_string(g.height) +
               " OffsetX=" + std::to_string(g.offset_x) + " OffsetY=" + std::to_string(g.offset_y) +
               " BinningHorizontal=" + std::to_string(g.binning_horizontal) +
               " BinningVertical=" + std::to_string(g.binning_vertical) +
               " (window " + std::to_string(g.WindowColumns()) + "x" + std::to_string(g.WindowRows()) + ")";
    }


    void ValidateSensorGeometry(const SensorGeometry &g, const std::string &what) {
        // Bounds keep the window products far from overflow.
        constexpr std::int64_t kLimit = std::int64_t{1} << 20;
        const auto in = [](std::int64_t value, std::int64_t lowest) { return value >= lowest && value <= kLimit; };
        if (!in(g.width, 1) || !in(g.height, 1) || !in(g.binning_horizontal, 1) || !in(g.binning_vertical, 1) ||
            !in(g.offset_x, 0) || !in(g.offset_y, 0)) {
            throw ConfigError(what + ": implausible geometry Width=" + std::to_string(g.width) +
                              " Height=" + std::to_string(g.height) + " OffsetX=" + std::to_string(g.offset_x) +
                              " OffsetY=" + std::to_string(g.offset_y) +
                              " BinningHorizontal=" + std::to_string(g.binning_horizontal) +
                              " BinningVertical=" + std::to_string(g.binning_vertical));
        }
    }


    bool SameSensorWindow(const SensorGeometry &a, const SensorGeometry &b) {
        return a.offset_x == b.offset_x && a.offset_y == b.offset_y &&
               a.WindowColumns() == b.WindowColumns() && a.WindowRows() == b.WindowRows();
    }


    SensorGeometry ParseEbusConfigGeometry(const std::string &text, const std::string &what) {
        // eBUS Player stores the device's feature values as
        // <device name=".." version=".."> <mac/> <communication/> <device>
        //   <parameter name="Width">1024</parameter> ... </device> </device>.
        // Only that inner section is searched: other sections reuse parameter names.
        constexpr std::string_view kOpen = "<device>";
        constexpr std::string_view kClose = "</device>";
        const auto open = text.find(kOpen);
        if (open == std::string::npos || text.find(kOpen, open + kOpen.size()) != std::string::npos) {
            throw ConfigError(what + ": expected exactly one <device> parameter section");
        }
        const auto body = open + kOpen.size();
        const auto close = text.find(kClose, body);
        if (close == std::string::npos) {
            throw ConfigError(what + ": unterminated <device> parameter section");
        }
        const std::string_view section(text.data() + body, close - body);

        const auto read = [&](const char *name) {
            const std::string tag = std::string("<parameter name=\"") + name + "\">";
            const auto at = section.find(tag);
            if (at == std::string_view::npos || section.find(tag, at + tag.size()) != std::string_view::npos) {
                throw ConfigError(what + ": device parameter '" + name + "' must appear exactly once");
            }
            const auto value_begin = at + tag.size();
            const auto value_end = section.find("</parameter>", value_begin);
            if (value_end == std::string_view::npos) {
                throw ConfigError(what + ": device parameter '" + name + "' is not terminated");
            }
            const std::string value = Trim(std::string(section.substr(value_begin, value_end - value_begin)));
            std::int64_t out = 0;
            const char *first = value.data();
            const char *last = value.data() + value.size();
            const auto [ptr, ec] = std::from_chars(first, last, out);
            if (value.empty() || ec != std::errc() || ptr != last) {
                throw ConfigError(what + ": device parameter '" + name + "' is not a decimal integer: '" + value + "'");
            }
            return out;
        };

        SensorGeometry geometry;
        geometry.width = read("Width");
        geometry.height = read("Height");
        geometry.offset_x = read("OffsetX");
        geometry.offset_y = read("OffsetY");
        geometry.binning_horizontal = read("BinningHorizontal");
        geometry.binning_vertical = read("BinningVertical");
        ValidateSensorGeometry(geometry, what);
        return geometry;
    }


    SensorGeometry ReadEbusConfigGeometry(const std::filesystem::path &pvcfg) {
        return ParseEbusConfigGeometry(ReadBytes(pvcfg, "device.ebus_config"),
                                       "device.ebus_config '" + pvcfg.string() + "'");
    }
} // namespace fx10
