#include "scan_verify.h"

#include <spdlog/fmt/fmt.h>


namespace lms4xxx {
    std::string VerifyScanContent(const ScanData &scan, const ScanConfig &scan_config, bool ntp_enabled) {
        std::string problems;
        const auto bad = [&problems](bool cond, const std::string &what) {
            if (cond) {
                problems += (problems.empty() ? "" : ", ") + what;
            }
        };
        const bool refl = scan_config.remission == Remission::kRefl;
        const char *remission_name = RemissionChannel(scan_config.remission);
        const auto *remission = refl ? scan.ReflectanceChannel() : scan.RssiChannel();
        bad(scan.DistanceChannel() == nullptr, "DIST1 missing");
        bad(remission == nullptr, std::string(remission_name) + " missing");
        bad(scan.AngleCorrectionChannel() == nullptr, "ANGL1 missing");
        bad(scan.QualityChannel() == nullptr, "QLTY1 missing");
        bad(scan.channels_16bit.size() != 3, fmt::format("{} 16-bit channels (expected 3)", scan.channels_16bit.size()));
        bad(scan.channels_8bit.size() != 1, fmt::format("{} 8-bit channels (expected 1)", scan.channels_8bit.size()));

        const auto check_geometry = [&bad](const char *name, std::uint16_t num, std::uint16_t step, std::int32_t start) {
            bad(num != ScanFixed::kPointsPerScan, fmt::format("{}: {} points (expected {})", name, num, ScanFixed::kPointsPerScan));
            bad(step != ScanFixed::kAngularResolution1e4,
                fmt::format("{}: step {} (expected {})", name, step, ScanFixed::kAngularResolution1e4));
            bad(start != ScanFixed::kStartAngle1e4,
                fmt::format("{}: start {} (expected {})", name, start, ScanFixed::kStartAngle1e4));
        };
        for (const auto &ch: scan.channels_16bit) {
            bad(ch.data.size() != ch.num_data, "16-bit channel payload count mismatch");
            const char *name = ch.content == ChannelContent16::kDist1   ? "DIST1"
                               : ch.content == ChannelContent16::kAngl1 ? "ANGL1"
                                                                        : remission_name;
            check_geometry(name, ch.num_data, ch.angle_step, ch.start_angle);
        }
        for (const auto &ch: scan.channels_8bit) {
            bad(ch.data.size() != ch.num_data, "8-bit channel payload count mismatch");
            check_geometry("QLTY1", ch.num_data, ch.angle_step, ch.start_angle);
        }
        if (ntp_enabled) {
            bad(!scan.has_timestamp, "timestamp block missing");
        } else {
            bad(scan.has_timestamp, "unexpected timestamp block (NTP off)"); // a free-running clock is not output
        }
        bad(scan.has_encoder, "unexpected encoder block");
        bad(scan.has_device_name, "unexpected device name block");
        return problems;
    }
} // namespace lms4xxx
