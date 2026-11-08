// ebus_config_file.cpp: serial/directory match, checksums.txt lookup, OpenSSL
// SHA-256, and the shipped .pvcfg. No SDK, no camera.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "ebus_config_file.h"

using namespace fx10;
namespace fs = std::filesystem;

namespace {
    bool Contains(const std::string &s, const std::string &needle) { return s.find(needle) != std::string::npos; }

    std::string ErrorOf(const fs::path &path) {
        try {
            CheckEbusConfigChecksum(path);
        } catch (const ConfigError &e) {
            return e.what();
        }
        return {};
    }

    struct TempDir {
        fs::path dir = fs::temp_directory_path() /
                       ("fx10_ebus_config_" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count())) / "077000060381";
        TempDir() { fs::create_directories(dir); }
        ~TempDir() {
            std::error_code ignored;
            fs::remove_all(dir.parent_path(), ignored);
        }
        fs::path Write(const std::string &name, const std::string &text) const {
            std::ofstream(dir / name, std::ios::binary) << text;
            return dir / name;
        }
    };

    // sha256("abc"), FIPS 180-4
    const std::string kAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
} // namespace

TEST_CASE("ebus_config: the serial must equal the directory name") {
    const fs::path p = "fx10_driver/resource/ebus_config/077000060381/3210798.pvcfg";
    CHECK_NOTHROW(CheckEbusConfigSerial(p, "077000060381"));
    CHECK_NOTHROW(CheckEbusConfigSerial(p, " 077000060381\n")); // camera string padding
    CHECK_THROWS_AS(CheckEbusConfigSerial(p, "077000060382"), ConfigError);
    CHECK_THROWS_AS(CheckEbusConfigSerial(p, ""), ConfigError);
    CHECK_THROWS_AS(CheckEbusConfigSerial("3210798.pvcfg", "077000060381"), ConfigError);
}

TEST_CASE("ebus_config: checksums.txt is required and must match the file") {
    TempDir t;
    const fs::path pvcfg = t.Write("cam.pvcfg", "abc");
    CHECK(Sha256File(pvcfg) == kAbc);
    CHECK(Contains(ErrorOf(pvcfg), "cannot open")); // no checksums.txt

    t.Write("checksums.txt", kAbc + "  cam.pvcfg\n");
    CHECK(CheckEbusConfigChecksum(pvcfg) == kAbc);
    t.Write("checksums.txt", "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD *cam.pvcfg\r\n");
    CHECK_NOTHROW(CheckEbusConfigChecksum(pvcfg)); // binary marker, CRLF, uppercase hex

    t.Write("checksums.txt", std::string(64, '0') + "  cam.pvcfg\n");
    CHECK(Contains(ErrorOf(pvcfg), "does not match"));
    t.Write("checksums.txt", kAbc + "  other.pvcfg\n");
    CHECK(Contains(ErrorOf(pvcfg), "found 0"));
    t.Write("checksums.txt", kAbc + "  cam.pvcfg\n" + kAbc + "  cam.pvcfg\n");
    CHECK(Contains(ErrorOf(pvcfg), "found 2"));

    t.Write("checksums.txt", kAbc + "  cam.pvcfg\n");
    t.Write("cam.pvcfg", "abd"); // one byte changed after the checksum was taken
    CHECK(Contains(ErrorOf(pvcfg), "does not match"));
}

TEST_CASE("ebus_config: the shipped file passes both checks") {
    const fs::path pvcfg = fs::path(FX10_CONFIG_DIR) / "../resource/ebus_config/077000060381/3210798.pvcfg";
    CHECK_NOTHROW(CheckEbusConfigSerial(pvcfg, "077000060381"));
    CHECK(CheckEbusConfigChecksum(pvcfg) == "4b2e39369fa3e9490c7ea30cff85366a2ad51d5b2c2654773eae508412ec7c70");
}
