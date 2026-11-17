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

namespace {
    const std::string kGeometryParameters =
            "<parameter name=\"Width\">1024</parameter>\n"
            "<parameter name=\"Height\"> 224 </parameter>\n"
            "<parameter name=\"WidthMax\">1312</parameter>\n" // a longer name must not match Width
            "<parameter name=\"OffsetX\">112</parameter>\n"
            "<parameter name=\"OffsetY\">285</parameter>\n"
            "<parameter name=\"BinningHorizontal\">1</parameter>\n"
            "<parameter name=\"BinningVertical\">2</parameter>\n";

    std::string Pvcfg(const std::string &device_parameters, const std::string &outside = "") {
        return "<puregevpersistencefile version=\"1.0\">\n"
               "<genparameterarray name=\"gevcommunication\" version=\"1.0\">" + outside + "</genparameterarray>\n"
               "<device name=\"\" version=\"1.0\"><mac>00:00:00:00:00:00</mac>\n"
               "<communication><parameter name=\"AnswerTimeout\">1000</parameter></communication>\n"
               "<device>\n" + device_parameters + "</device>\n</device>\n</puregevpersistencefile>\n";
    }

    std::string GeometryError(const std::string &text) {
        try {
            ParseEbusConfigGeometry(text, "test.pvcfg");
        } catch (const ConfigError &e) {
            return e.what();
        }
        return {};
    }

    std::string Replace(std::string text, const std::string &from, const std::string &to) {
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        return text.replace(at, from.size(), to);
    }
} // namespace

TEST_CASE("ebus_config geometry: the shipped file carries the Specim calibration window") {
    const fs::path pvcfg = fs::path(FX10_CONFIG_DIR) / "../resource/ebus_config/077000060381/3210798.pvcfg";
    const SensorGeometry g = ReadEbusConfigGeometry(pvcfg);
    CHECK(g.width == 1024);
    CHECK(g.height == 224);
    CHECK(g.offset_x == 112);
    CHECK(g.offset_y == 285);
    CHECK(g.binning_horizontal == 1);
    CHECK(g.binning_vertical == 2);
    CHECK(g.WindowColumns() == 1024);
    CHECK(g.WindowRows() == 448); // 224 bands at 2x spectral binning (manual Table 10)
}

TEST_CASE("ebus_config geometry: only the single device parameter section is read, strictly") {
    const SensorGeometry g = ParseEbusConfigGeometry(Pvcfg(kGeometryParameters), "test.pvcfg");
    CHECK(g == SensorGeometry{1024, 224, 112, 285, 1, 2});

    // Present only outside the device section
    const std::string no_height = Replace(kGeometryParameters, "<parameter name=\"Height\"> 224 </parameter>\n", "");
    CHECK(Contains(GeometryError(Pvcfg(no_height, "<parameter name=\"Height\">224</parameter>")),
                   "'Height' must appear exactly once"));
    CHECK(Contains(GeometryError(Pvcfg(kGeometryParameters + "<parameter name=\"Width\">1024</parameter>\n")),
                   "'Width' must appear exactly once"));
    CHECK(Contains(GeometryError(Pvcfg(Replace(kGeometryParameters, ">2<", ">2.0<"))),
                   "'BinningVertical' is not a decimal integer"));
    CHECK(Contains(GeometryError(Pvcfg(Replace(kGeometryParameters, ">285<", "><"))),
                   "'OffsetY' is not a decimal integer"));
    CHECK(Contains(GeometryError(Pvcfg(Replace(kGeometryParameters, ">1024<", ">0<"))), "implausible geometry"));
    CHECK(Contains(GeometryError(Pvcfg(Replace(kGeometryParameters, ">112<", ">-1<"))), "implausible geometry"));
    CHECK(Contains(GeometryError(Pvcfg(kGeometryParameters) + "<device></device>"), "exactly one <device>"));
    CHECK(Contains(GeometryError("<device name=\"\"><device>" + kGeometryParameters), "unterminated"));
}

TEST_CASE("ebus_config geometry: the detector window survives a binning change, nothing else") {
    const SensorGeometry restored{1024, 224, 112, 285, 1, 2};
    CHECK(SameSensorWindow(restored, restored));
    CHECK(SameSensorWindow(restored, SensorGeometry{1024, 448, 112, 285, 1, 1}));
    CHECK(SameSensorWindow(restored, SensorGeometry{512, 56, 112, 285, 2, 8}));
    // Height written under binning 1, then rescaled by BinningVertical = 2
    CHECK_FALSE(SameSensorWindow(restored, SensorGeometry{1024, 112, 112, 285, 1, 2}));
    CHECK_FALSE(SameSensorWindow(restored, SensorGeometry{1024, 224, 112, 286, 1, 2}));
    CHECK_FALSE(SameSensorWindow(restored, SensorGeometry{1024, 224, 0, 285, 1, 2}));
    CHECK(Contains(FormatSensorGeometry(restored), "Height=224"));
    CHECK(Contains(FormatSensorGeometry(restored), "(window 1024x448)"));
}
