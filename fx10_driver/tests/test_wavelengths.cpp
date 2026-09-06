#include "../include/wavelengths.h"

#include <doctest/doctest.h>
#include <unistd.h>

#include <cmath>
#include <fstream>
#include <string>

using fx10::ConfigError;
using fx10::ResolveWavelengths;
using fx10::WavelengthConfig;
using fx10::WavelengthSource;

namespace {
    std::string WriteTempFile(const std::string &name, const std::string &content) {
        const std::string path =
                (std::filesystem::temp_directory_path() / "fx10_wl_").string() + std::to_string(::getpid()) + "_" + name;
        std::ofstream out(path, std::ios::trunc);
        out << content;
        return path;
    }
} // namespace

TEST_CASE("Wavelengths: GridEndpointsExactAndMonotonic") {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kGrid; // pure interpolation only; acquisition rejects generated grids
    const auto wl = ResolveWavelengths(cfg, 224);
    REQUIRE(wl.nm.size() == 224u);
    CHECK(wl.nm.front() == doctest::Approx(400.0));
    CHECK(wl.nm.back() == doctest::Approx(1000.0));
    for (std::size_t i = 1; i < wl.nm.size(); ++i) {
        CHECK(wl.nm[i - 1] < wl.nm[i]);
    }
    CHECK(wl.source_tag == "generated-linear");
    CHECK(wl.fwhm.empty());
}

TEST_CASE("Wavelengths: GridSingleBand") {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kGrid;
    const auto wl = ResolveWavelengths(cfg, 1);
    REQUIRE(wl.nm.size() == 1u);
    CHECK(wl.nm[0] == doctest::Approx(400.0));
}

TEST_CASE("Wavelengths: ListSourceAndCountMismatch") {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kList;
    cfg.list = {400.0, 500.0, 600.0};
    const auto wl = ResolveWavelengths(cfg, 3);
    CHECK(wl.nm == cfg.list);
    CHECK(wl.source_tag == "list");
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 4), ConfigError);
}

TEST_CASE("Wavelengths: FwhmListValidated") {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kList;
    cfg.list = {400.0, 500.0};
    cfg.fwhm_list = {5.5, 5.5};
    CHECK(ResolveWavelengths(cfg, 2).fwhm == cfg.fwhm_list);
    cfg.fwhm_list = {5.5};
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 2), ConfigError);
}

TEST_CASE("Wavelengths: FileOneColumnWithCommentsAndCommas") {
    const std::string path = WriteTempFile("one_col.txt",
                                           "# calibration pack\n"
                                           "400.5\n"
                                           "500.5, 600.5\n" // commas act as separators
                                           "\n");
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kFile;
    cfg.file = path;
    // "500.5, 600.5" is a 2-number line while "400.5" is 1-number -> inconsistent.
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 3), ConfigError);

    const std::string clean = WriteTempFile("one_col_clean.txt", "400.5\n500.5\n600.5 # last\n");
    cfg.file = clean;
    const auto wl = ResolveWavelengths(cfg, 3);
    REQUIRE(wl.nm.size() == 3u);
    CHECK(wl.nm[1] == doctest::Approx(500.5));
    CHECK(wl.fwhm.empty());
    CHECK(wl.source_tag == "file:" + clean);
}

TEST_CASE("Wavelengths: FileTwoColumnsYieldFwhm") {
    const std::string path =
            WriteTempFile("two_col.txt", "400.0 5.4\n500.0 5.5\n600.0 5.6\n");
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kFile;
    cfg.file = path;
    const auto wl = ResolveWavelengths(cfg, 3);
    REQUIRE(wl.fwhm.size() == 3u);
    CHECK(wl.fwhm[2] == doctest::Approx(5.6));
}

TEST_CASE("Wavelengths: FileErrors") {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kFile;
    cfg.file = "/nonexistent/wl.txt";
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 3), ConfigError);

    cfg.file = WriteTempFile("count.txt", "400\n500\n");
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 3), ConfigError); // 2 values, 3 bands

    cfg.file = WriteTempFile("three_col.txt", "400 5.5 9\n");
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 1), ConfigError); // 3 numbers on a line

    cfg.file = WriteTempFile("units_col.txt", "400.0 nm\n");
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 1), ConfigError); // trailing garbage token
}

TEST_CASE("Wavelengths: RejectsNonFiniteAndOutOfRangeValues") {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kList;
    cfg.list = {std::nan("")};
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 1), ConfigError);
    cfg.list = {-400.0};
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 1), ConfigError);
    cfg.list = {1e30};
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 1), ConfigError);

    cfg.list = {500.0};
    cfg.fwhm_list = {std::nan("")};
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 1), ConfigError);
}

TEST_CASE("Wavelengths: ExplicitFwhmListOverridesFileColumn") {
    const std::string path = WriteTempFile("override.txt", "400 1.0\n500 1.0\n");
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kFile;
    cfg.file = path;
    cfg.fwhm_list = {7.0, 7.0};
    const auto wl = ResolveWavelengths(cfg, 2);
    CHECK(wl.fwhm[0] == doctest::Approx(7.0));
}

TEST_CASE("Wavelengths: CalibrationRequiresMatchingDeviceAndDeliveredROI") {
    WavelengthConfig cfg;
    CHECK(fx10::ResolveForGeometry(cfg, "camera-A", 1312, 541, 0, 0, false).nm.empty());
    cfg.source = WavelengthSource::kGrid;
    CHECK_THROWS_AS(fx10::ResolveForGeometry(cfg, "camera-A", 4, 3, 8, 10, false), ConfigError);
    cfg.source = WavelengthSource::kList;
    cfg.list = {410.1256, 511.9876, 613.2468};
    CHECK_THROWS_AS(fx10::ResolveForGeometry(cfg, "camera-A", 4, 3, 8, 10, false), ConfigError);
    cfg.calibration = {"pack-A-rev1", "camera-A", 4, 3, 8, 10};
    CHECK(fx10::ResolveForGeometry(cfg, "camera-A", 4, 3, 8, 10, false).nm == cfg.list);
    CHECK_THROWS_AS(fx10::ResolveForGeometry(cfg, "camera-B", 4, 3, 8, 10, false), ConfigError);
    CHECK_THROWS_AS(fx10::ResolveForGeometry(cfg, "camera-A", 4, 541, 8, 10, false), ConfigError);
    CHECK_THROWS_AS(fx10::ResolveForGeometry(cfg, "camera-A", 4, 3, 0, 0, false), ConfigError);
    CHECK_THROWS_AS(fx10::ResolveForGeometry(cfg, "camera-A", 4, 3, 8, 10, true), ConfigError);
    cfg.source = WavelengthSource::kFile;
    cfg.file = WriteTempFile("bad_row.txt", "410\nBROKEN\n511\n613\n");
    CHECK_THROWS_AS(ResolveWavelengths(cfg, 3), ConfigError);
}
