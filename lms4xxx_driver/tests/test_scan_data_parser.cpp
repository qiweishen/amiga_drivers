/// @file test_scan_data_parser.cpp
/// @brief ScanDataParser against synthesised sSN LMDscandata payloads.
///
/// The parser is 400 lines of hand-written offset arithmetic on data a device
/// controls, and it had no test and no fixture at all. The builder below lays
/// the telegram out field by field from the manual's own table (Operating
/// Instructions 8023198, "Send data permanently [sEN LMDscandata]", p.91-95),
/// so the expectations come from SICK rather than from the implementation.

#include <ostream>

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "error.h"
#include "scan_data_parser.h"

using lms4xxx::ChannelContent16;
using lms4xxx::ChannelContent8;
using lms4xxx::ErrorCode;
using lms4xxx::ScanData;
namespace ScanDataParser = lms4xxx::ScanDataParser;

namespace {
    // --- big-endian writers (manual p.69: values are output MSB first) --------
    void PutU8(std::vector<std::uint8_t> &out, std::uint8_t v) { out.push_back(v); }

    void PutU16(std::vector<std::uint8_t> &out, std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v));
    }

    void PutU32(std::vector<std::uint8_t> &out, std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v >> 24));
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v));
    }

    void PutI32(std::vector<std::uint8_t> &out, std::int32_t v) { PutU32(out, static_cast<std::uint32_t>(v)); }

    void PutFloat(std::vector<std::uint8_t> &out, float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        PutU32(out, bits);
    }

    void PutName(std::vector<std::uint8_t> &out, const char *five) {
        for (int i = 0; i < 5; ++i) {
            out.push_back(static_cast<std::uint8_t>(five[i]));
        }
    }

    // One 16-bit measured-value channel (manual p.93).
    struct Channel16 {
        const char *content = "DIST1";
        float scale = 0.1f; ///< DIST1 is 1/10 mm, so the documented factor is 0.1
        float offset = 0.0f;
        std::int32_t start_angle = 550000; ///< +55 deg in 1/10000 deg (86470h)
        std::uint16_t step = 833; ///< 1/12 deg in 1/10000 deg (341h)
        std::vector<std::uint16_t> data;
    };

    struct Channel8 {
        const char *content = "QLTY1";
        float scale = 1.0f;
        float reserved = 0.0f;
        std::int32_t start_angle = 550000;
        std::uint16_t step = 833;
        std::vector<std::uint8_t> data;
    };

    // Everything after "sSN LMDscandata ", i.e. what ColaBCodec::Decode hands to
    // the parser. Field order and widths follow the manual's table exactly.
    struct Telegram {
        std::uint16_t version = 1;
        std::uint16_t device_number = 0x1234;
        std::uint32_t serial_number = 1605397;
        std::uint8_t status_1 = 0;
        std::uint8_t status_2 = 0;
        std::uint16_t telegram_counter = 0x51DA;
        std::uint16_t scan_counter = 0xC2AC;
        std::uint32_t since_startup_us = 0xF918'14F0;
        std::uint32_t transmission_us = 0xF918'181E;
        std::uint8_t din_1 = 0, din_2 = 0, dout_1 = 0, dout_2 = 0;
        std::uint32_t scan_frequency = 60000; ///< 600 Hz in 1/100 Hz (EA60h)
        std::uint32_t measurement_frequency = 0x21C0;
        std::vector<std::uint32_t> encoders; ///< positions; empty = "amount of encoder" 0
        std::vector<Channel16> ch16;
        std::vector<Channel8> ch8;
        float y_rotation = 0.0f;
        bool has_name = false;
        std::string name;
        bool has_time = false;
        std::uint16_t year = 2026;
        std::uint8_t month = 9, day = 4, hour = 12, minute = 34, second = 56;
        std::uint32_t microsecond = 123456;
        bool trailing_reserved = true;

        std::vector<std::uint8_t> Build() const {
            std::vector<std::uint8_t> p;
            PutU16(p, version);
            PutU16(p, device_number);
            PutU32(p, serial_number);
            PutU8(p, status_1);
            PutU8(p, status_2);
            PutU16(p, telegram_counter);
            PutU16(p, scan_counter);
            PutU32(p, since_startup_us);
            PutU32(p, transmission_us);
            PutU8(p, din_1);
            PutU8(p, din_2);
            PutU8(p, dout_1);
            PutU8(p, dout_2);
            PutU16(p, 0); // reserved

            PutU32(p, scan_frequency);
            PutU32(p, measurement_frequency);

            PutU16(p, static_cast<std::uint16_t>(encoders.size()));
            for (const auto position: encoders) {
                PutU32(p, position);
                PutU16(p, 0); // reserved
            }

            PutU16(p, static_cast<std::uint16_t>(ch16.size()));
            for (const auto &c: ch16) {
                PutName(p, c.content);
                PutFloat(p, c.scale);
                PutFloat(p, c.offset);
                PutI32(p, c.start_angle);
                PutU16(p, c.step);
                PutU16(p, static_cast<std::uint16_t>(c.data.size()));
                for (const auto v: c.data) {
                    PutU16(p, v);
                }
            }

            PutU16(p, static_cast<std::uint16_t>(ch8.size()));
            for (const auto &c: ch8) {
                PutName(p, c.content);
                PutFloat(p, c.scale);
                PutFloat(p, c.reserved);
                PutI32(p, c.start_angle);
                PutU16(p, c.step);
                PutU16(p, static_cast<std::uint16_t>(c.data.size()));
                for (const auto v: c.data) {
                    PutU8(p, v);
                }
            }

            PutFloat(p, y_rotation);

            PutU16(p, has_name ? 1 : 0);
            if (has_name) {
                PutU16(p, static_cast<std::uint16_t>(name.size()));
                for (const char ch: name) {
                    PutU8(p, static_cast<std::uint8_t>(ch));
                }
            }

            PutU16(p, has_time ? 1 : 0);
            if (has_time) {
                PutU16(p, year);
                PutU8(p, month);
                PutU8(p, day);
                PutU8(p, hour);
                PutU8(p, minute);
                PutU8(p, second);
                PutU32(p, microsecond);
            }

            if (trailing_reserved) {
                PutU16(p, 0);
            }
            return p;
        }
    };

    // The shape this driver configures: DIST1 + RSSI1 + ANGL1 + QLTY1
    // (LMDscandatacfg "further data channels" = 7, manual p.85).
    Telegram FullyConfiguredTelegram(std::uint16_t points = 4) {
        Telegram t;
        Channel16 dist;
        dist.content = "DIST1";
        dist.scale = 0.1f;
        Channel16 rssi;
        rssi.content = "RSSI1";
        rssi.scale = 1.0f;
        Channel16 angl;
        angl.content = "ANGL1";
        angl.scale = 1.0f;
        angl.offset = -32768.0f; // ANGL1 carries the -3.2768 deg offset (p.93)
        Channel8 qlty;
        for (std::uint16_t i = 0; i < points; ++i) {
            dist.data.push_back(static_cast<std::uint16_t>(1000 + i));
            rssi.data.push_back(static_cast<std::uint16_t>(200 + i));
            angl.data.push_back(static_cast<std::uint16_t>(32768 + i));
            qlty.data.push_back(static_cast<std::uint8_t>(0x10 + i));
        }
        t.ch16 = {dist, rssi, angl};
        t.ch8 = {qlty};
        return t;
    }

    std::error_code ParseOf(const std::vector<std::uint8_t> &payload, ScanData &out) {
        return ScanDataParser::Parse(payload.data(), payload.size(), out, "test");
    }
} // namespace


TEST_CASE("ScanDataParser decodes a fully configured telegram") {
    const auto payload = FullyConfiguredTelegram().Build();
    ScanData scan;
    REQUIRE(!ParseOf(payload, scan));

    CHECK(scan.device_info.version_number == 1);
    CHECK(scan.device_info.device_number == 0x1234);
    CHECK(scan.device_info.serial_number == 1605397u);
    CHECK(scan.device_info.device_status_1 == lms4xxx::DeviceStatus::kOk);
    CHECK(scan.telegram_counter == 0x51DA);
    CHECK(scan.scan_counter == 0xC2AC);
    CHECK(scan.time_since_startup_us == 0xF91814F0u);
    CHECK(scan.transmission_time_us == 0xF918181Eu);

    // 60000 in 1/100 Hz = the 600 Hz the manual quotes for the LMS4000 (p.92)
    CHECK(scan.scan_frequency == 60000u);
    CHECK(scan.ScanFrequencyHz() == doctest::Approx(600.0f));

    CHECK_FALSE(scan.has_encoder);
    CHECK_FALSE(scan.has_device_name);
    CHECK_FALSE(scan.has_timestamp);

    REQUIRE(scan.channels_16bit.size() == 3);
    REQUIRE(scan.channels_8bit.size() == 1);

    const auto *dist = scan.DistanceChannel();
    REQUIRE(dist != nullptr);
    CHECK(dist->content == ChannelContent16::kDist1);
    CHECK(dist->scale_factor == doctest::Approx(0.1f));
    CHECK(dist->start_angle == 550000);
    CHECK(dist->angle_step == 833);
    CHECK(dist->num_data == 4);
    CHECK(dist->data[0] == 1000);
    CHECK(dist->data[3] == 1003);
    // DIST1 is in 1/10 mm: 1000 raw = 100.0 mm
    CHECK(dist->ScaledValue(0) == doctest::Approx(100.0f));

    REQUIRE(scan.RssiChannel() != nullptr);
    CHECK(scan.RssiChannel()->data[1] == 201);
    CHECK(scan.ReflectanceChannel() == nullptr); // rssi and refl are mutually exclusive

    const auto *angl = scan.AngleCorrectionChannel();
    REQUIRE(angl != nullptr);
    // 32768 with offset -32768 is exactly 0 deg of correction (manual p.21)
    CHECK(angl->ScaledValue(0) == doctest::Approx(0.0f));

    const auto *qlty = scan.QualityChannel();
    REQUIRE(qlty != nullptr);
    CHECK(qlty->content == ChannelContent8::kQlty1);
    CHECK(qlty->num_data == 4);
    CHECK(qlty->data[0] == 0x10); // bit 4 = normal measuring point (p.21)

    CHECK(scan.NumPoints() == 4);
}

TEST_CASE("ScanDataParser reads the optional encoder, name and timestamp blocks") {
    Telegram t = FullyConfiguredTelegram();
    t.encoders = {0xDEADBEEF};
    t.has_name = true;
    t.name = "Front_Center";
    t.has_time = true;
    ScanData scan;
    REQUIRE(!ParseOf(t.Build(), scan));

    CHECK(scan.has_encoder);
    CHECK(scan.encoder.position == 0xDEADBEEFu);
    CHECK(scan.has_device_name);
    CHECK(scan.device_name == "Front_Center");
    REQUIRE(scan.has_timestamp);
    CHECK(scan.timestamp.year == 2026);
    CHECK(scan.timestamp.month == 9);
    CHECK(scan.timestamp.day == 4);
    CHECK(scan.timestamp.hour == 12);
    CHECK(scan.timestamp.minute == 34);
    CHECK(scan.timestamp.second == 56);
    CHECK(scan.timestamp.microsecond == 123456u);
}

TEST_CASE("ScanDataParser consumes every encoder block, not just the first") {
    // Two blocks used to leave six bytes in the buffer, which shifted every
    // later section and surfaced as a bogus "frame too short" elsewhere.
    Telegram t = FullyConfiguredTelegram();
    t.encoders = {111, 222};
    t.has_time = true;
    ScanData scan;
    REQUIRE(!ParseOf(t.Build(), scan));
    CHECK(scan.encoder.position == 111u); // the first one is what is reported
    CHECK(scan.has_timestamp); // ...and the rest of the telegram still lines up
    CHECK(scan.timestamp.year == 2026);
}

TEST_CASE("ScanDataParser rejects device-controlled counts that are out of range") {
    SUBCASE("too many 16-bit channels") {
        auto payload = FullyConfiguredTelegram().Build();
        // The 16-bit channel count sits right after the 28-byte device block,
        // the 8-byte frequency block and the 2-byte encoder count.
        constexpr std::size_t kCountOffset = 28 + 8 + 2;
        payload[kCountOffset] = 0x00;
        payload[kCountOffset + 1] = 0x05; // max is 4 (DIST/RSSI/REFL/ANGL)
        ScanData scan;
        CHECK(ParseOf(payload, scan) == make_error_code(ErrorCode::kProtocolError));
    }
    SUBCASE("too many points in a channel") {
        Telegram t = FullyConfiguredTelegram();
        auto payload = t.Build();
        // num_data of the first channel: count(2) + name(5) + scale(4) +
        // offset(4) + start(4) + step(2)
        constexpr std::size_t kNumDataOffset = 28 + 8 + 2 + 2 + 5 + 4 + 4 + 4 + 2;
        payload[kNumDataOffset] = 0x03;
        payload[kNumDataOffset + 1] = 0x4A; // 842 > the 841 the aperture allows
        ScanData scan;
        CHECK(ParseOf(payload, scan) == make_error_code(ErrorCode::kProtocolError));
    }
    SUBCASE("device name longer than the telegram allows") {
        Telegram t = FullyConfiguredTelegram();
        t.has_name = true;
        t.name = "0123456789ABCDEFG"; // 17 > 16
        ScanData scan;
        CHECK(ParseOf(t.Build(), scan) == make_error_code(ErrorCode::kProtocolError));
    }
}

TEST_CASE("ScanDataParser rejects a truncation at every block boundary") {
    const auto full = FullyConfiguredTelegram().Build();
    // Every prefix shorter than the whole telegram must be refused, and none of
    // them may read past the end: the Reader primitives are unguarded, so this
    // is the only thing standing between a short frame and an out-of-bounds
    // read. (Run under ASan this doubles as the memory-safety test.)
    // The last field is the optional trailing reserved u16, so the two prefixes
    // that only drop it are legitimately complete telegrams.
    for (std::size_t len = 0; len + 2 < full.size(); ++len) {
        ScanData scan;
        const auto ec = ScanDataParser::Parse(full.data(), len, scan, "test");
        CHECK_MESSAGE(static_cast<bool>(ec), "prefix of length " << len << " was accepted");
    }
    ScanData scan;
    CHECK(!ScanDataParser::Parse(full.data(), full.size(), scan, "test"));
}

TEST_CASE("ScanDataParser refuses a payload below the documented minimum") {
    std::vector<std::uint8_t> tiny(51, 0);
    ScanData scan;
    CHECK(ParseOf(tiny, scan) == make_error_code(ErrorCode::kFrameTooShort));
}

TEST_CASE("ScanDataParser keeps unknown channel names as data") {
    // An unrecognised content string must not be silently dropped: the points
    // still parse, and the channel is simply marked unknown so the recorder can
    // ignore it while the counters stay honest.
    Telegram t = FullyConfiguredTelegram();
    t.ch16[1].content = "XXXX1";
    ScanData scan;
    REQUIRE(!ParseOf(t.Build(), scan));
    REQUIRE(scan.channels_16bit.size() == 3);
    CHECK(scan.channels_16bit[1].content == ChannelContent16::kUnknown);
    CHECK(scan.channels_16bit[1].num_data == 4);
    CHECK(scan.RssiChannel() == nullptr);
}

TEST_CASE("A scan with no measured-value channels still parses") {
    // The device sends this while the laser is off (manual p.19): the header is
    // intact and every channel count is zero. It must decode, so the driver's
    // first-frame verification can report exactly what is missing.
    Telegram t;
    ScanData scan;
    REQUIRE(!ParseOf(t.Build(), scan));
    CHECK(scan.channels_16bit.empty());
    CHECK(scan.channels_8bit.empty());
    CHECK(scan.NumPoints() == 0);
    CHECK(scan.DistanceChannel() == nullptr);
}

TEST_CASE("Reserved distance values have the meanings the manual assigns") {
    // Manual p.95: valid measurements start at 16; below that the value is a
    // status code, not a distance. Anything that averages or thresholds the
    // channel has to know this.
    CHECK(lms4xxx::InvalidDistance::kInvalidDark == 0);
    CHECK(lms4xxx::InvalidDistance::kInvalidBright == 1);
    CHECK(lms4xxx::InvalidDistance::kReservedMax == 15);

    Telegram t = FullyConfiguredTelegram(4);
    t.ch16[0].data = {0, 1, 15, 16};
    ScanData scan;
    REQUIRE(!ParseOf(t.Build(), scan));
    const auto *dist = scan.DistanceChannel();
    REQUIRE(dist != nullptr);
    CHECK(dist->data[0] == lms4xxx::InvalidDistance::kInvalidDark);
    CHECK(dist->data[1] == lms4xxx::InvalidDistance::kInvalidBright);
    CHECK(dist->data[2] <= lms4xxx::InvalidDistance::kReservedMax);
    CHECK(dist->data[3] > lms4xxx::InvalidDistance::kReservedMax); // the first valid value
}

TEST_CASE("Quality bits match the manual's table") {
    // Manual p.21/p.94. These are the flags a post-processing step filters on,
    // so a renumbering here would silently change which points are trusted.
    CHECK(lms4xxx::Quality::kBelowSignalLower == 0x01);
    CHECK(lms4xxx::Quality::kAboveSignalUpper == 0x02);
    CHECK(lms4xxx::Quality::kBelowDistLower == 0x04);
    CHECK(lms4xxx::Quality::kAboveDistUpper == 0x08);
    CHECK(lms4xxx::Quality::kNormalMeasurement == 0x10);
    CHECK(lms4xxx::Quality::kEdgeHitPossible == 0x20);
    CHECK(lms4xxx::Quality::kEdgeHitLikely == 0x30);
    CHECK(lms4xxx::Quality::kSuspectedOutlier == 0x40);
    CHECK(lms4xxx::Quality::kPartialGloss == 0x80);
}

TEST_CASE("ScanDataParser refuses a telegram with bytes left over") {
    // A layout that differs from the assumed one shows up as unparsed bytes, not as success
    auto payload = FullyConfiguredTelegram().Build();
    payload.push_back(0xEE);
    ScanData scan;
    CHECK(ParseOf(payload, scan) == make_error_code(ErrorCode::kProtocolError));
}

TEST_CASE("ScanDataParser bounds the encoder count") {
    Telegram t = FullyConfiguredTelegram();
    t.encoders = {1, 2, 3};
    ScanData scan;
    CHECK(ParseOf(t.Build(), scan) == make_error_code(ErrorCode::kProtocolError));
}
