#include "../include/wavelengths.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cmath>
#include <fstream>
#include <string>

using fx10::ConfigError;
using fx10::resolveWavelengths;
using fx10::WavelengthConfig;
using fx10::WavelengthSource;

namespace {
    std::string writeTempFile(const std::string &name, const std::string &content) {
        const std::string path =
                ::testing::TempDir() + "fx10_wl_" + std::to_string(::getpid()) + "_" + name;
        std::ofstream out(path, std::ios::trunc);
        out << content;
        return path;
    }
} // namespace

TEST(Wavelengths, GridEndpointsExactAndMonotonic) {
    WavelengthConfig cfg; // defaults: grid 400..1000
    const auto wl = resolveWavelengths(cfg, 224);
    ASSERT_EQ(wl.nm.size(), 224u);
    EXPECT_DOUBLE_EQ(wl.nm.front(), 400.0);
    EXPECT_DOUBLE_EQ(wl.nm.back(), 1000.0);
    for (std::size_t i = 1; i < wl.nm.size(); ++i) {
        EXPECT_LT(wl.nm[i - 1], wl.nm[i]);
    }
    EXPECT_EQ(wl.source_tag, "generated-linear");
    EXPECT_TRUE(wl.fwhm.empty());
}

TEST(Wavelengths, GridSingleBand) {
    WavelengthConfig cfg;
    const auto wl = resolveWavelengths(cfg, 1);
    ASSERT_EQ(wl.nm.size(), 1u);
    EXPECT_DOUBLE_EQ(wl.nm[0], 400.0);
}

TEST(Wavelengths, ListSourceAndCountMismatch) {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kList;
    cfg.list = {400.0, 500.0, 600.0};
    const auto wl = resolveWavelengths(cfg, 3);
    EXPECT_EQ(wl.nm, cfg.list);
    EXPECT_EQ(wl.source_tag, "list");
    EXPECT_THROW(resolveWavelengths(cfg, 4), ConfigError);
}

TEST(Wavelengths, FwhmListValidated) {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kList;
    cfg.list = {400.0, 500.0};
    cfg.fwhm_list = {5.5, 5.5};
    EXPECT_EQ(resolveWavelengths(cfg, 2).fwhm, cfg.fwhm_list);
    cfg.fwhm_list = {5.5};
    EXPECT_THROW(resolveWavelengths(cfg, 2), ConfigError);
}

TEST(Wavelengths, FileOneColumnWithCommentsAndCommas) {
    const std::string path = writeTempFile("one_col.txt",
                                           "# calibration pack\n"
                                           "400.5\n"
                                           "500.5, 600.5\n" // commas act as separators
                                           "\n");
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kFile;
    cfg.file = path;
    // "500.5, 600.5" is a 2-number line while "400.5" is 1-number -> inconsistent.
    EXPECT_THROW(resolveWavelengths(cfg, 3), ConfigError);

    const std::string clean = writeTempFile("one_col_clean.txt", "400.5\n500.5\n600.5 # last\n");
    cfg.file = clean;
    const auto wl = resolveWavelengths(cfg, 3);
    ASSERT_EQ(wl.nm.size(), 3u);
    EXPECT_DOUBLE_EQ(wl.nm[1], 500.5);
    EXPECT_TRUE(wl.fwhm.empty());
    EXPECT_EQ(wl.source_tag, "file:" + clean);
}

TEST(Wavelengths, FileTwoColumnsYieldFwhm) {
    const std::string path =
            writeTempFile("two_col.txt", "400.0 5.4\n500.0 5.5\n600.0 5.6\n");
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kFile;
    cfg.file = path;
    const auto wl = resolveWavelengths(cfg, 3);
    ASSERT_EQ(wl.fwhm.size(), 3u);
    EXPECT_DOUBLE_EQ(wl.fwhm[2], 5.6);
}

TEST(Wavelengths, FileErrors) {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kFile;
    cfg.file = "/nonexistent/wl.txt";
    EXPECT_THROW(resolveWavelengths(cfg, 3), ConfigError);

    cfg.file = writeTempFile("count.txt", "400\n500\n");
    EXPECT_THROW(resolveWavelengths(cfg, 3), ConfigError); // 2 values, 3 bands

    cfg.file = writeTempFile("three_col.txt", "400 5.5 9\n");
    EXPECT_THROW(resolveWavelengths(cfg, 1), ConfigError); // 3 numbers on a line

    cfg.file = writeTempFile("units_col.txt", "400.0 nm\n");
    EXPECT_THROW(resolveWavelengths(cfg, 1), ConfigError); // trailing garbage token
}

TEST(Wavelengths, RejectsNonFiniteAndOutOfRangeValues) {
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kList;
    cfg.list = {std::nan("")};
    EXPECT_THROW(resolveWavelengths(cfg, 1), ConfigError);
    cfg.list = {-400.0};
    EXPECT_THROW(resolveWavelengths(cfg, 1), ConfigError);
    cfg.list = {1e30};
    EXPECT_THROW(resolveWavelengths(cfg, 1), ConfigError);

    cfg.list = {500.0};
    cfg.fwhm_list = {std::nan("")};
    EXPECT_THROW(resolveWavelengths(cfg, 1), ConfigError);
}

TEST(Wavelengths, ExplicitFwhmListOverridesFileColumn) {
    const std::string path = writeTempFile("override.txt", "400 1.0\n500 1.0\n");
    WavelengthConfig cfg;
    cfg.source = WavelengthSource::kFile;
    cfg.file = path;
    cfg.fwhm_list = {7.0, 7.0};
    const auto wl = resolveWavelengths(cfg, 2);
    EXPECT_DOUBLE_EQ(wl.fwhm[0], 7.0);
}
