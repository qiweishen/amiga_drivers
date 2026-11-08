#include "ebus_config_file.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

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
} // namespace fx10
