#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "app_config.h"


// CoLa B telegrams the driver sends (manual annex 12.3); every builder returns a complete frame
namespace lms4xxx::CommandBuilder {
    // sMN SetAccessMode: Authorized Client (level 3, password F4724744, manual p.72)
    std::vector<std::uint8_t> BuildLogin();

    // sMN Run (activate parameters, log out)
    std::vector<std::uint8_t> BuildRun();

    // sWN LMDscandatacfg. Defaults = the driver's fixed setup: DIST + remission + ANGL + QLTY
    // (further 0x07), no encoder, no device name, every scan; time stamp only with NTP
    std::vector<std::uint8_t> BuildScanDataConfig(Remission unit, bool time, std::uint8_t further_channels = 0x07,
                                                  bool encoder = false, bool device_name = false,
                                                  std::uint16_t output_rate = ScanFixed::kOutputRate);

    // sWN LMPoutputRange (1/10000 deg); defaults = ScanFixed aperture
    std::vector<std::uint8_t> BuildOutputRange(std::uint32_t resolution_1e4 = ScanFixed::kAngularResolution1e4,
                                               std::int32_t start_1e4 = ScanFixed::kStartAngle1e4,
                                               std::int32_t stop_1e4 = ScanFixed::kStopAngle1e4);

    // sEN LMDscandata 1 / 0
    std::vector<std::uint8_t> BuildStartStream();

    std::vector<std::uint8_t> BuildStopStream();

    // sRN <name>
    std::vector<std::uint8_t> BuildReadVariable(std::string_view name);

    // sMN LMCstartmeas / LMCstandby
    std::vector<std::uint8_t> BuildStartMeasurement();

    std::vector<std::uint8_t> BuildStandby();

    // sMN mSCloadappdef: application defaults, interface/IP settings untouched (manual p.79)
    std::vector<std::uint8_t> BuildLoadAppDefaults();

    // sWN TSCRole (0 = Off, 1 = Client, 2 = Server)
    std::vector<std::uint8_t> BuildSetTimeSyncRole(std::uint8_t role);

    // sWN TSCTCSrvAddr / TSCTCupdatetime / TSCTCtimezone
    std::vector<std::uint8_t> BuildSetNtpServer(const std::array<std::uint8_t, 4> &ip_bytes);

    std::vector<std::uint8_t> BuildSetNtpUpdateTime(std::uint32_t seconds);

    std::vector<std::uint8_t> BuildSetNtpTimezone(std::uint8_t timezone);

    // Filters (all written off; manual sections 3.5.8.x)
    std::vector<std::uint8_t> BuildMeanFilter(bool enable, std::uint16_t num_scans);

    std::vector<std::uint8_t> BuildMedianFilter(bool enable);

    std::vector<std::uint8_t> BuildFrontendEdgeFilter(std::uint8_t status); // raw byte, see ScanFixed

    std::vector<std::uint8_t> BuildEdgeFilter(bool enable);

    std::vector<std::uint8_t> BuildCubicAreaFilter(std::uint8_t status, std::uint32_t min_dist, std::uint32_t max_dist,
                                                   std::int32_t neg_expansion, std::int32_t pos_expansion); // 1/10 mm

    std::vector<std::uint8_t> BuildGlossFilter(bool enable);
} // namespace lms4xxx::CommandBuilder
