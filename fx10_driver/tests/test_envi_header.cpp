#include "envi_header.h"

#include <doctest/doctest.h>

#include <stdexcept>

using fx10::EnviDataType;
using fx10::EnviHeaderInfo;
using fx10::GenerateEnviHeader;

namespace {
    EnviHeaderInfo SmallInfo() {
        EnviHeaderInfo info;
        info.samples = 4;
        info.lines = 10;
        info.bands = 3;
        info.data_type = EnviDataType::kUint16;
        info.description = "Test run";
        info.acquisition_time_iso = "2026-07-21T00:00:00Z";
        info.wavelengths_nm = {400.0, 700.0, 1000.0};
        info.fwhm_nm = {5.5, 5.5, 5.5};
        return info;
    }
} // namespace

TEST_CASE("EnviHeader: GoldenText") {
    const std::string expected =
            "ENVI\n"
            "description = {\n"
            "Test run}\n"
            "samples = 4\n"
            "lines = 10\n"
            "bands = 3\n"
            "header offset = 0\n"
            "file type = ENVI Standard\n"
            "data type = 12\n"
            "interleave = bil\n"
            "byte order = 0\n"
            "reference acquisition start time = 2026-07-21T00:00:00Z\n"
            "wavelength units = Nanometers\n"
            "wavelength = {\n"
            " 400, 700, 1000\n"
            "}\n"
            "fwhm = {\n"
            " 5.5, 5.5, 5.5\n"
            "}\n";
    CHECK(GenerateEnviHeader(SmallInfo()) == expected);
}

TEST_CASE("EnviHeader: WrapsEightValuesPerLine") {
    EnviHeaderInfo info = SmallInfo();
    info.bands = 10;
    info.wavelengths_nm = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    info.fwhm_nm.clear();
    const std::string text = GenerateEnviHeader(info);
    CHECK(std::string(text).find(" 7, 8,\n 9, 10\n}\n") != std::string::npos);
}

TEST_CASE("EnviHeader: CalibrationValuesRoundTripWithoutDecimalRounding") {
    auto info = SmallInfo();
    info.wavelengths_nm[0] = 400.1234567890123;
    const auto text = GenerateEnviHeader(info);
    const auto start = text.find("wavelength = {\n");
    REQUIRE(start != std::string::npos);
    CHECK(std::stod(text.substr(start + std::string("wavelength = {\n").size())) == info.wavelengths_nm[0]);
}

TEST_CASE("EnviHeader: OmitsOptionalBlocks") {
    EnviHeaderInfo info = SmallInfo();
    info.wavelengths_nm.clear();
    info.fwhm_nm.clear();
    info.acquisition_time_iso.clear();
    const std::string text = GenerateEnviHeader(info);
    CHECK(text.find("wavelength") == std::string::npos);
    CHECK(text.find("fwhm") == std::string::npos);
    // The key is spelled "reference acquisition start time": asserting the
    // absence of "acquisition time" used to test nothing, because that
    // substring never appears in the output either way.
    CHECK(text.find("reference acquisition start time") == std::string::npos);
    // Positive control for the assertion above — the same info WITH a time
    // must contain exactly that key.
    info.acquisition_time_iso = "2026-07-21T00:00:00Z";
    CHECK(std::string(GenerateEnviHeader(info)).find("reference acquisition start time = 2026-07-21T00:00:00Z\n") != std::string::npos);
}

TEST_CASE("EnviHeader: SanitizesBracesInDescription") {
    EnviHeaderInfo info = SmallInfo();
    info.description = "gain{2} mode={x}";
    CHECK(std::string(GenerateEnviHeader(info)).find("gain(2) mode=(x)") != std::string::npos);
}

TEST_CASE("EnviHeader: Uint8DataType") {
    EnviHeaderInfo info = SmallInfo();
    info.data_type = EnviDataType::kUint8;
    CHECK(std::string(GenerateEnviHeader(info)).find("data type = 1\n") != std::string::npos);
}

TEST_CASE("EnviHeader: Validation") {
    EnviHeaderInfo info = SmallInfo();
    info.wavelengths_nm = {400.0}; // 1 value, 3 bands
    CHECK_THROWS_AS(GenerateEnviHeader(info), std::invalid_argument);

    info = SmallInfo();
    info.fwhm_nm = {5.5};
    CHECK_THROWS_AS(GenerateEnviHeader(info), std::invalid_argument);

    info = SmallInfo();
    info.samples = 0;
    CHECK_THROWS_AS(GenerateEnviHeader(info), std::invalid_argument);
}
