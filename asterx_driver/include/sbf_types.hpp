// *****************************************************************************
//
// Parsing logic derived from septentrio_gnss_driver 1.4.7
// parsers/sbf_blocks.hpp (BSD-3-Clause), rewritten from boost::spirit::qi to
// bounds-checked little-endian reads; ROS axis flips removed (native
// Septentrio conventions). Full license: 3rd_party/septentrio_gnss_driver/LICENSE.
//
// *****************************************************************************

#pragma once

#include <cstdint>
#include <limits>
#include <vector>


namespace asterx::sbf {
    // Block IDs (raw id field & 8191)
    inline constexpr std::uint16_t kIdPVTGeodetic = 4007;
    inline constexpr std::uint16_t kIdReceiverStatus = 4014;
    inline constexpr std::uint16_t kIdExtSensorMeas = 4050;
    inline constexpr std::uint16_t kIdINSNavGeod = 4226;
    inline constexpr std::uint16_t kIdExtEventINSNavGeod = 4230; // same layout/parser as 4226
    inline constexpr std::uint16_t kIdAttEuler = 5938;

    // Do-Not-Use sentinels (SBF spec; floats carry -2e10 on the wire and are
    // converted to NaN at read time, integers keep the sentinel value)
    inline constexpr std::uint32_t kDnuU32 = 4294967295u; // tow
    inline constexpr std::uint16_t kDnuU16 = 65535; // wnc / u16 fields
    inline constexpr std::uint8_t kDnuU8 = 255; // nr_sv
    inline constexpr float kDnuFloat = -2e10f;
    inline constexpr double kDnuDouble = -2e10;

    inline float DnuToNan(float v) { return v == kDnuFloat ? std::numeric_limits<float>::quiet_NaN() : v; }
    inline double DnuToNan(double v) { return v == kDnuDouble ? std::numeric_limits<double>::quiet_NaN() : v; }
    inline bool Valid(std::uint8_t v) { return v != kDnuU8; }
    inline bool Valid(std::uint16_t v) { return v != kDnuU16; }
    inline bool Valid(std::uint32_t v) { return v != kDnuU32; }

    // SBF latitude/longitude are radians; the live CSV contract is degrees
    inline constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

    // GPS time -> Unix nanoseconds (GPS epoch 1980-01-06 = Unix 315964800 s).
    // GPS-UTC leap seconds: 18 since 2017-01-01, unchanged as of 2026-07; bump
    // this constant when IERS announces a new leap second. Returns 0 when
    // tow/wnc carry the DNU sentinel (caller falls back to the host timestamp).
    inline constexpr std::int64_t kGpsUtcLeapSeconds = 18;

    inline std::uint64_t GpsToUnixNs(std::uint32_t tow_ms, std::uint16_t wnc) {
        if (!Valid(tow_ms) || !Valid(wnc)) {
            return 0;
        }
        return 315964800ull * 1000000000ull + static_cast<std::uint64_t>(tow_ms) * 1000000ull +
               static_cast<std::uint64_t>(wnc) * 604800ull * 1000000000ull -
               static_cast<std::uint64_t>(kGpsUtcLeapSeconds) * 1000000000ull;
    }

    namespace detail {
        inline constexpr float kNanF = std::numeric_limits<float>::quiet_NaN();
        inline constexpr double kNanD = std::numeric_limits<double>::quiet_NaN();
    } // namespace detail

    // Wire layout: every block starts with this 14-byte header ($@ sync, CRC
    // over bytes [4, length), 13-bit id + 3-bit revision, total length, GPS time)
    struct BlockHeader {
        std::uint8_t sync_1{0}; // 0x24 '$'
        std::uint8_t sync_2{0}; // 0x40 '@'
        std::uint16_t crc{0};
        std::uint16_t id{0}; // raw & 8191
        std::uint8_t revision{0}; // raw >> 13
        std::uint16_t length{0}; // whole frame, multiple of 4
        std::uint32_t tow{kDnuU32}; // ms since GPS week start
        std::uint16_t wnc{kDnuU16}; // GPS week number
    };

    // Field order below mirrors the wire order (reference PVTGeodeticParser).
    // Defaults are the DNU representation, so fields a low-revision frame does
    // not carry keep their Do-Not-Use semantics after a successful parse.
    struct PVTGeodetic {
        // 4007
        BlockHeader block_header;
        std::uint8_t mode{0}; // low 4 bits = PVT solution type
        std::uint8_t error{0}; // 0 = OK
        double latitude{detail::kNanD}; // rad
        double longitude{detail::kNanD}; // rad
        double height{detail::kNanD}; // m, ellipsoidal
        float undulation{detail::kNanF}; // m
        float vn{detail::kNanF}; // m/s
        float ve{detail::kNanF}; // m/s
        float vu{detail::kNanF}; // m/s
        float cog{detail::kNanF}; // deg
        double rx_clk_bias{detail::kNanD}; // ms
        float rx_clk_drift{detail::kNanF}; // ppm
        std::uint8_t time_system{0};
        std::uint8_t datum{0};
        std::uint8_t nr_sv{kDnuU8};
        std::uint8_t wa_corr_info{0};
        std::uint16_t reference_id{kDnuU16};
        std::uint16_t mean_corr_age{kDnuU16}; // 0.01 s
        std::uint32_t signal_info{0};
        std::uint8_t alert_flag{0};
        std::uint8_t nr_bases{0}; // revision > 0
        std::uint16_t ppp_info{0}; // revision > 0, s
        std::uint16_t latency{kDnuU16}; // revision > 1, 0.0001 s
        std::uint16_t h_accuracy{kDnuU16}; // revision > 1, 0.01 m
        std::uint16_t v_accuracy{kDnuU16}; // revision > 1, 0.01 m
        std::uint8_t misc{0}; // revision > 1
    };

    struct ExtSensorMeas {
        // 4050; one 28-byte sub-block per measurement set
        BlockHeader block_header;
        std::uint8_t n{0};
        std::uint8_t sb_length{0}; // must be 28
        std::vector<std::uint8_t> source, sensor_model, type, obs_info;
        double acceleration_x{detail::kNanD}, acceleration_y{detail::kNanD},
                acceleration_z{detail::kNanD}; // type 0, m/s^2
        double angular_rate_x{detail::kNanD}, angular_rate_y{detail::kNanD},
                angular_rate_z{detail::kNanD}; // type 1, deg/s
        float velocity_x{detail::kNanF}, velocity_y{detail::kNanF}, velocity_z{detail::kNanF}; // type 4, m/s
        float std_dev_x{detail::kNanF}, std_dev_y{detail::kNanF}, std_dev_z{detail::kNanF}; // type 4, m/s
        float sensor_temperature{detail::kNanF}; // type 3, degC (wire int16/100, sentinel -32768)
        double zero_velocity_flag{detail::kNanD}; // type 20
        bool has_imu_meas{false}; // acceleration AND angular rate present
    };

    struct INSNavGeod {
        // 4226 / 4230; optional sub-blocks gated by sb_list bits
        BlockHeader block_header;
        std::uint8_t gnss_mode{0}; // same encoding as PVTGeodetic::mode
        std::uint8_t error{0};
        std::uint16_t info{0};
        std::uint16_t gnss_age{kDnuU16}; // 0.01 s
        double latitude{detail::kNanD}; // rad
        double longitude{detail::kNanD}; // rad
        double height{detail::kNanD}; // m
        float undulation{detail::kNanF}; // m
        std::uint16_t accuracy{kDnuU16}; // 0.01 m
        std::uint16_t latency{kDnuU16}; // 0.0001 s
        std::uint8_t datum{0};
        std::uint16_t sb_list{0};
        // Wire order of the optional groups is the bit order 1..128 (the .msg
        // declaration order in the reference differs from the wire — trust this)
        float latitude_std_dev{detail::kNanF}, longitude_std_dev{detail::kNanF},
                height_std_dev{detail::kNanF}; // bit 0, m
        float heading{detail::kNanF}, pitch{detail::kNanF}, roll{detail::kNanF}; // bit 1, deg
        float heading_std_dev{detail::kNanF}, pitch_std_dev{detail::kNanF},
                roll_std_dev{detail::kNanF}; // bit 2, deg
        float ve{detail::kNanF}, vn{detail::kNanF}, vu{detail::kNanF}; // bit 3, m/s
        float ve_std_dev{detail::kNanF}, vn_std_dev{detail::kNanF},
                vu_std_dev{detail::kNanF}; // bit 4, m/s
        float latitude_longitude_cov{detail::kNanF}, latitude_height_cov{detail::kNanF},
                longitude_height_cov{detail::kNanF}; // bit 5, m^2
        float heading_pitch_cov{detail::kNanF}, heading_roll_cov{detail::kNanF},
                pitch_roll_cov{detail::kNanF}; // bit 6, deg^2
        float ve_vn_cov{detail::kNanF}, ve_vu_cov{detail::kNanF},
                vn_vu_cov{detail::kNanF}; // bit 7, m^2/s^2
    };

    struct AgcState {
        std::uint8_t frontend_id{0};
        std::int8_t gain{0}; // dB
        std::uint8_t sample_var{0};
        std::uint8_t blanking_stat{0}; // %
    };

    struct ReceiverStatus {
        // 4014
        BlockHeader block_header;
        std::uint8_t cpu_load{0}; // %
        std::uint8_t ext_error{0}; // bitmask
        std::uint32_t up_time{0}; // s
        std::uint32_t rx_status{0}; // bitmask, bit 8 = WARN
        std::uint32_t rx_error{0}; // bitmask, non-zero = error
        std::uint8_t n{0}; // AGC sub-blocks, max 18
        std::uint8_t sb_length{0};
        std::uint8_t cmd_count{0};
        std::uint8_t temperature{0}; // wire = degC + 100
        std::vector<AgcState> agc_state;
    };

    struct AttEuler {
        // 5938
        BlockHeader block_header;
        std::uint8_t nr_sv{kDnuU8};
        std::uint8_t error{0};
        std::uint16_t mode{0};
        float heading{detail::kNanF}; // deg
        float pitch{detail::kNanF}; // deg
        float roll{detail::kNanF}; // deg
        float pitch_dot{detail::kNanF}; // deg/s
        float roll_dot{detail::kNanF}; // deg/s
        float heading_dot{detail::kNanF}; // deg/s
    };
} // namespace asterx::sbf
