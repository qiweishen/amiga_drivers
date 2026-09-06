#include "command_builder.h"

#include "cola_b.h"
#include "scan_data.h"


namespace lms4xxx::CommandBuilder {
    static_assert(ScanFixed::kPointsPerScan == kMaxPointsPerScan, "recorder width must match the device aperture");

    namespace {
        constexpr std::uint8_t kAuthorizedClientLevel = 0x03;
        constexpr std::uint32_t kAuthorizedClientPassword = 0xF4724744;

        void Append16(std::vector<std::uint8_t> &out, std::uint16_t v) {
            const auto b = ColaBCodec::EncodeUint16(v);
            out.insert(out.end(), b.begin(), b.end());
        }

        void Append32(std::vector<std::uint8_t> &out, std::uint32_t v) {
            const auto b = ColaBCodec::EncodeUint32(v);
            out.insert(out.end(), b.begin(), b.end());
        }

        std::uint8_t Flag(bool on) { return on ? 0x01 : 0x00; }
    } // namespace


    std::vector<std::uint8_t> BuildLogin() {
        std::vector<std::uint8_t> params = {kAuthorizedClientLevel};
        Append32(params, kAuthorizedClientPassword);
        return ColaBCodec::Encode(CommandType::kMethodByName, "SetAccessMode", params);
    }

    std::vector<std::uint8_t> BuildRun() {
        return ColaBCodec::Encode(CommandType::kMethodByName, "Run");
    }

    std::vector<std::uint8_t> BuildScanDataConfig(Remission unit, bool time, std::uint8_t further_channels,
                                                  bool encoder, bool device_name, std::uint16_t output_rate) {
        // Data channel Uint8 Uint8 (01 00 = distance) | Further Uint8 | Reserved 01 | Unit Enum8 |
        // Encoder Uint8 Uint8 | Reserved 00 | Device name Bool | Reserved 00 | Time Bool | Output rate Uint16
        std::vector<std::uint8_t> params = {
            0x01, 0x00, further_channels, 0x01, static_cast<std::uint8_t>(unit),
            Flag(encoder), 0x00, 0x00, Flag(device_name), 0x00, Flag(time),
        };
        Append16(params, output_rate);
        return ColaBCodec::Encode(CommandType::kWriteByName, "LMDscandatacfg", params);
    }

    std::vector<std::uint8_t> BuildOutputRange(std::uint32_t resolution_1e4, std::int32_t start_1e4,
                                               std::int32_t stop_1e4) {
        // Reserved Int16 = 0001 | Resolution Uint32 | Start Int32 | Stop Int32
        std::vector<std::uint8_t> params;
        Append16(params, 0x0001);
        Append32(params, resolution_1e4);
        Append32(params, static_cast<std::uint32_t>(start_1e4));
        Append32(params, static_cast<std::uint32_t>(stop_1e4));
        return ColaBCodec::Encode(CommandType::kWriteByName, "LMPoutputRange", params);
    }

    std::vector<std::uint8_t> BuildStartStream() {
        return ColaBCodec::Encode(CommandType::kEventByName, "LMDscandata", {0x01});
    }

    std::vector<std::uint8_t> BuildStopStream() {
        return ColaBCodec::Encode(CommandType::kEventByName, "LMDscandata", {0x00});
    }

    std::vector<std::uint8_t> BuildReadVariable(std::string_view name) {
        return ColaBCodec::Encode(CommandType::kReadByName, name);
    }

    std::vector<std::uint8_t> BuildStartMeasurement() {
        return ColaBCodec::Encode(CommandType::kMethodByName, "LMCstartmeas");
    }

    std::vector<std::uint8_t> BuildStandby() {
        return ColaBCodec::Encode(CommandType::kMethodByName, "LMCstandby");
    }

    std::vector<std::uint8_t> BuildLoadAppDefaults() {
        return ColaBCodec::Encode(CommandType::kMethodByName, "mSCloadappdef");
    }

    std::vector<std::uint8_t> BuildSetTimeSyncRole(std::uint8_t role) {
        return ColaBCodec::Encode(CommandType::kWriteByName, "TSCRole", {role});
    }

    std::vector<std::uint8_t> BuildSetNtpServer(const std::array<std::uint8_t, 4> &ip_bytes) {
        return ColaBCodec::Encode(CommandType::kWriteByName, "TSCTCSrvAddr",
                                  std::vector<std::uint8_t>(ip_bytes.begin(), ip_bytes.end()));
    }

    std::vector<std::uint8_t> BuildSetNtpUpdateTime(std::uint32_t seconds) {
        std::vector<std::uint8_t> params;
        Append32(params, seconds);
        return ColaBCodec::Encode(CommandType::kWriteByName, "TSCTCupdatetime", params);
    }

    std::vector<std::uint8_t> BuildSetNtpTimezone(std::uint8_t timezone) {
        return ColaBCodec::Encode(CommandType::kWriteByName, "TSCTCtimezone", {timezone});
    }

    std::vector<std::uint8_t> BuildMeanFilter(bool enable, std::uint16_t num_scans) {
        // status Bool | num_scans Uint16 (2..100) | reserved Enum8 = 0
        std::vector<std::uint8_t> params = {Flag(enable)};
        Append16(params, num_scans);
        params.push_back(0x00);
        return ColaBCodec::Encode(CommandType::kWriteByName, "LFPmeanfilter", params);
    }

    std::vector<std::uint8_t> BuildMedianFilter(bool enable) {
        // status Bool | reserved Uint16 = 0003
        std::vector<std::uint8_t> params = {Flag(enable)};
        Append16(params, 0x0003);
        return ColaBCodec::Encode(CommandType::kWriteByName, "LFPmedianfilter", params);
    }

    std::vector<std::uint8_t> BuildFrontendEdgeFilter(std::uint8_t status) {
        // status Bool | reserved Enum8 = 1
        return ColaBCodec::Encode(CommandType::kWriteByName, "LFPfrontendEdgefilter", {status, 0x01});
    }

    std::vector<std::uint8_t> BuildEdgeFilter(bool enable) {
        return ColaBCodec::Encode(CommandType::kWriteByName, "LFPedgefilter", {Flag(enable)});
    }

    std::vector<std::uint8_t> BuildCubicAreaFilter(std::uint8_t status, std::uint32_t min_dist, std::uint32_t max_dist,
                                                   std::int32_t neg_expansion, std::int32_t pos_expansion) {
        // status Bool | min Uint32 | max Uint32 | neg_exp Int32 | pos_exp Int32
        std::vector<std::uint8_t> params = {status};
        Append32(params, min_dist);
        Append32(params, max_dist);
        Append32(params, static_cast<std::uint32_t>(neg_expansion));
        Append32(params, static_cast<std::uint32_t>(pos_expansion));
        return ColaBCodec::Encode(CommandType::kWriteByName, "LFPcubicareafilter", params);
    }

    std::vector<std::uint8_t> BuildGlossFilter(bool enable) {
        return ColaBCodec::Encode(CommandType::kWriteByName, "LFPglossfilter", {Flag(enable)});
    }
} // namespace lms4xxx::CommandBuilder
