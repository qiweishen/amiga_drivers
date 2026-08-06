#include "envi_header.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "envi_recorder.hpp"

using ::testing::HasSubstr;
using fx10::EnviDataType;
using fx10::EnviHeaderInfo;
using fx10::generateEnviHeader;

namespace {
    EnviHeaderInfo smallInfo() {
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

TEST(EnviHeader, GoldenText) {
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
            " 400.000, 700.000, 1000.000\n"
            "}\n"
            "fwhm = {\n"
            " 5.500, 5.500, 5.500\n"
            "}\n";
    EXPECT_EQ(generateEnviHeader(smallInfo()), expected);
}

TEST(EnviHeader, WrapsEightValuesPerLine) {
    EnviHeaderInfo info = smallInfo();
    info.bands = 10;
    info.wavelengths_nm = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    info.fwhm_nm.clear();
    const std::string text = generateEnviHeader(info);
    EXPECT_THAT(text, HasSubstr(" 7.000, 8.000,\n 9.000, 10.000\n}\n"));
}

TEST(EnviHeader, OmitsOptionalBlocks) {
    EnviHeaderInfo info = smallInfo();
    info.wavelengths_nm.clear();
    info.fwhm_nm.clear();
    info.acquisition_time_iso.clear();
    const std::string text = generateEnviHeader(info);
    EXPECT_EQ(text.find("wavelength"), std::string::npos);
    EXPECT_EQ(text.find("fwhm"), std::string::npos);
    EXPECT_EQ(text.find("acquisition time"), std::string::npos);
}

TEST(EnviHeader, SanitizesBracesInDescription) {
    EnviHeaderInfo info = smallInfo();
    info.description = "gain{2} mode={x}";
    EXPECT_THAT(generateEnviHeader(info), HasSubstr("gain(2) mode=(x)"));
}

TEST(EnviHeader, Uint8DataType) {
    EnviHeaderInfo info = smallInfo();
    info.data_type = EnviDataType::kUint8;
    EXPECT_THAT(generateEnviHeader(info), HasSubstr("data type = 1\n"));
}

TEST(EnviHeader, Validation) {
    EnviHeaderInfo info = smallInfo();
    info.wavelengths_nm = {400.0}; // 1 value, 3 bands
    EXPECT_THROW(generateEnviHeader(info), fx10::RecorderError);

    info = smallInfo();
    info.fwhm_nm = {5.5};
    EXPECT_THROW(generateEnviHeader(info), fx10::RecorderError);

    info = smallInfo();
    info.samples = 0;
    EXPECT_THROW(generateEnviHeader(info), fx10::RecorderError);
}
