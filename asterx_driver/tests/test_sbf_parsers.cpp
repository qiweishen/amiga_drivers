// Synthetic-frame tests for the live SBF parsers. Frames are hand-built in
// wire order (little endian); the CRC field is a placeholder — ssnrx validates
// CRC before the parsers ever see a frame, so they do not re-check it.
//
// Only the two blocks the driver interprets are modelled: INSNavGeod (live CSV)
// and ReceiverStatus (warm-up gate). Everything else goes to .sbf untouched.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "sbf_frame_builder.h"
#include "sbf_parsers.h"

using namespace asterx::sbf;

using asterx_test::FrameBuilder;
using asterx_test::ins_fixed_body;
using asterx_test::rxs_fixed_body;


TEST_CASE("BlockHeader: Unpack") {
    FrameBuilder fb;
    const auto f = fb.finalize(4226, 2, 987654, 2345);
    BlockHeader h;
    REQUIRE(ParseBlockHeader(f.data(), f.size(), h));
    CHECK(h.id == 4226);
    CHECK(h.revision == 2);
    CHECK(h.tow == 987654u);
    CHECK(h.wnc == 2345);
    CHECK(PeekId(f.data(), f.size()) == 4226);

    auto bad = f;
    bad[0] = 0x25; // wrong sync byte 1
    CHECK_FALSE(ParseBlockHeader(bad.data(), bad.size(), h));

    // A buffer shorter than the id field has no id to peek at
    CHECK(PeekId(f.data(), 4) == 0);
}


TEST_CASE("Ins: SbListEmpty") {
    FrameBuilder fb;
    ins_fixed_body(fb);
    fb.U16(0); // sb_list
    const auto f = fb.finalize(4226, 0);
    INSNavGeod m;
    REQUIRE(ParseINSNavGeod(f.data(), f.size(), m));
    CHECK(m.latitude == doctest::Approx(0.55));
    CHECK(m.gnss_age == 5);
    CHECK(m.accuracy == 8);
    CHECK(std::isnan(m.heading));
    CHECK(std::isnan(m.ve));
    CHECK(std::isnan(m.latitude_std_dev));
    CHECK(std::isnan(m.vn_vu_cov));
}


TEST_CASE("Ins: SbListPartialWireOrderIsBitOrder") {
    // bit1 (attitude) + bit3 (velocity): the wire carries attitude FIRST, then
    // velocity — regression for the reference .msg declaration-order trap
    FrameBuilder fb;
    ins_fixed_body(fb);
    fb.U16(0x0A); // bits 1 and 3
    fb.F32(90.5f);
    fb.F32(1.25f);
    fb.F32(-0.5f); // heading, pitch, roll
    fb.F32(0.5f);
    fb.F32(0.25f);
    fb.F32(-0.125f); // ve, vn, vu
    const auto f = fb.finalize(4226, 0);
    INSNavGeod m;
    REQUIRE(ParseINSNavGeod(f.data(), f.size(), m));
    CHECK(m.heading == doctest::Approx(90.5f));
    CHECK(m.roll == doctest::Approx(-0.5f));
    CHECK(m.ve == doctest::Approx(0.5f));
    CHECK(m.vu == doctest::Approx(-0.125f));
    CHECK(std::isnan(m.heading_std_dev)); // absent groups stay NaN
    CHECK(std::isnan(m.latitude_std_dev));
}


TEST_CASE("Ins: SbListFullAndEventId") {
    FrameBuilder fb;
    ins_fixed_body(fb);
    fb.U16(0xFF); // all eight groups
    for (int i = 0; i < 24; ++i) fb.F32(static_cast<float>(i) + 0.5f);
    const auto f = fb.finalize(4230, 0); // ExtEventINSNavGeod shares the layout
    INSNavGeod m;
    REQUIRE(ParseINSNavGeod(f.data(), f.size(), m));
    CHECK(m.latitude_std_dev == doctest::Approx(0.5f)); // group bit0, first value
    CHECK(m.heading == doctest::Approx(3.5f)); // group bit1 starts at index 3
    CHECK(m.vn_vu_cov == doctest::Approx(23.5f)); // last value of group bit7
}


TEST_CASE("Ins: DnuAndTruncationAndWrongId") {
    FrameBuilder fb;
    fb.u8(0);
    fb.u8(24); // error: waiting for GNSS PVT
    fb.U16(0);
    fb.U16(65535); // gnss_age DNU
    fb.F64(-2e10); // latitude DNU
    fb.F64(-2e10);
    fb.F64(-2e10);
    fb.F32(-2e10f); // undulation DNU
    fb.U16(65535); // accuracy DNU
    fb.U16(65535); // latency DNU
    fb.u8(0);
    fb.u8(0);
    fb.U16(0);
    const auto f = fb.finalize(4226, 0);
    INSNavGeod m;
    REQUIRE(ParseINSNavGeod(f.data(), f.size(), m));
    CHECK(std::isnan(m.latitude));
    CHECK(std::isnan(m.undulation));
    CHECK_FALSE(Valid(m.gnss_age));
    CHECK_FALSE(Valid(m.accuracy));

    auto cut = f;
    cut.resize(30); // mid-body; must fail without reading past the buffer
    CHECK_FALSE(ParseINSNavGeod(cut.data(), cut.size(), m));

    // A frame whose sb_list promises groups the frame does not carry
    FrameBuilder liar;
    ins_fixed_body(liar);
    liar.U16(0xFF);
    const auto short_groups = liar.finalize(4226, 0);
    CHECK_FALSE(ParseINSNavGeod(short_groups.data(), short_groups.size(), m));

    const auto wrong = fb.finalize(4014, 0);
    CHECK_FALSE(ParseINSNavGeod(wrong.data(), wrong.size(), m));
}


TEST_CASE("Rxs: TwoAgcSubBlocksAreDistinct") {
    // Regression for the reference AgcStateParser taking its iterator by value
    // (every AGC sub-block read the same bytes there)
    FrameBuilder fb;
    rxs_fixed_body(fb, /*n=*/2, /*sb_length=*/8);
    fb.u8(1);
    fb.i8(50);
    fb.u8(10);
    fb.u8(0); // AGC 1
    fb.u8(0);
    fb.u8(0);
    fb.u8(0);
    fb.u8(0); // padding
    fb.u8(2);
    fb.i8(-3);
    fb.u8(20);
    fb.u8(5); // AGC 2
    fb.u8(0);
    fb.u8(0);
    fb.u8(0);
    fb.u8(0); // padding
    const auto f = fb.finalize(4014, 0);

    ReceiverStatus m;
    REQUIRE(ParseReceiverStatus(f.data(), f.size(), m));
    CHECK(m.cpu_load == 35);
    CHECK(m.temperature == 142);
    CHECK(m.up_time == 3600u);
    REQUIRE(m.agc_state.size() == 2u);
    CHECK(m.agc_state[0].frontend_id == 1);
    CHECK(m.agc_state[0].gain == 50);
    CHECK(m.agc_state[1].frontend_id == 2);
    CHECK(m.agc_state[1].gain == -3);
}


TEST_CASE("Rxs: ManyAgcSubBlocksAreAccepted") {
    // The reference guide states no maximum for N (p.373); the frame length is
    // the only bound. An arbitrary cap here would stall the warm-up gate, which
    // needs UpTime out of exactly this block.
    FrameBuilder fb;
    rxs_fixed_body(fb, /*n=*/20, /*sb_length=*/4);
    for (int i = 0; i < 20; ++i) {
        fb.u8(static_cast<std::uint8_t>(i));
        fb.i8(static_cast<std::int8_t>(-i));
        fb.u8(0);
        fb.u8(0);
    }
    const auto f = fb.finalize(4014, 0);

    ReceiverStatus m;
    REQUIRE(ParseReceiverStatus(f.data(), f.size(), m));
    REQUIRE(m.agc_state.size() == 20u);
    CHECK(m.agc_state[19].frontend_id == 19);
    CHECK(m.agc_state[19].gain == -19);
}


TEST_CASE("Rxs: FinetimeBitAndDnuFields") {
    FrameBuilder fb;
    // RxState bit 6 FINETIME (p.374); bit 8 is INTERNALDISK_FULL, not a warning
    rxs_fixed_body(fb, /*n=*/0, /*sb_length=*/0, /*up_time=*/1201, /*rx_status=*/(1u << 6) | (1u << 8));
    const auto f = fb.finalize(4014, 0);
    ReceiverStatus m;
    REQUIRE(ParseReceiverStatus(f.data(), f.size(), m));
    CHECK(m.up_time == 1201u);
    CHECK((m.rx_status & (1u << 6)) != 0u);
    CHECK(m.agc_state.empty());

    // CPULoad DNU is 255, Temperature DNU is 0 (p.373/p.375)
    FrameBuilder dnu;
    dnu.u8(255); // cpu_load DNU
    dnu.u8(0);
    dnu.U32(10);
    dnu.U32(0);
    dnu.U32(0);
    dnu.u8(0);
    dnu.u8(0);
    dnu.u8(0); // cmd_count DNU
    dnu.u8(0); // temperature DNU
    const auto fd = dnu.finalize(4014, 0);
    ReceiverStatus d;
    REQUIRE(ParseReceiverStatus(fd.data(), fd.size(), d));
    CHECK_FALSE(Valid(d.cpu_load));
    CHECK(d.temperature == 0);
}


TEST_CASE("Rxs: BadSbLengthAndTruncationRejected") {
    FrameBuilder fb;
    rxs_fixed_body(fb, /*n=*/1, /*sb_length=*/2); // sb_length < 4 would need a negative padding skip
    fb.u8(1);
    fb.u8(2);
    const auto f = fb.finalize(4014, 0);
    ReceiverStatus m;
    CHECK_FALSE(ParseReceiverStatus(f.data(), f.size(), m));

    // N promises more sub-blocks than the frame carries
    FrameBuilder liar;
    rxs_fixed_body(liar, /*n=*/4, /*sb_length=*/8);
    const auto short_frame = liar.finalize(4014, 0);
    CHECK_FALSE(ParseReceiverStatus(short_frame.data(), short_frame.size(), m));

    // Wrong block id
    FrameBuilder other;
    rxs_fixed_body(other, 0, 0);
    const auto wrong = other.finalize(4226, 0);
    CHECK_FALSE(ParseReceiverStatus(wrong.data(), wrong.size(), m));
}


TEST_CASE("Time: GpsToUnixNs") {
    // tow=0, wnc=0 -> GPS epoch minus leap seconds
    CHECK(GpsToUnixNs(0, 0) == 315964800ull * 1000000000ull - 18ull * 1000000000ull);
    // one week + 1.5 s into week 1
    CHECK(GpsToUnixNs(1500, 1) == 315964800ull * 1000000000ull + 604800ull * 1000000000ull + 1500000000ull - 18000000000ull);
    // DNU -> 0 (caller falls back to the host timestamp)
    CHECK(GpsToUnixNs(4294967295u, 100) == 0u);
    CHECK(GpsToUnixNs(1000, 65535) == 0u);
}

TEST_CASE("Header: the declared length bounds the parse") {
    FrameBuilder fb;
    ins_fixed_body(fb);
    fb.U16(1); // sb_list bit 0: the std-dev group follows
    fb.F32(1.0f);
    fb.F32(2.0f);
    fb.F32(3.0f);
    auto f = fb.finalize(4226, 0);
    INSNavGeod m;
    REQUIRE(ParseINSNavGeod(f.data(), f.size(), m));
    CHECK(m.latitude_std_dev == doctest::Approx(1.0f));

    // A declared length past the buffer is a truncated frame
    auto too_long = f;
    const std::uint16_t claimed = static_cast<std::uint16_t>(f.size() + 4);
    too_long[6] = static_cast<std::uint8_t>(claimed & 0xFF);
    too_long[7] = static_cast<std::uint8_t>(claimed >> 8);
    CHECK_FALSE(ParseINSNavGeod(too_long.data(), too_long.size(), m));

    // A declared length that excludes the group must not read it from the padding
    auto too_short = f;
    const std::uint16_t without_group = static_cast<std::uint16_t>(f.size() - 12);
    too_short[6] = static_cast<std::uint8_t>(without_group & 0xFF);
    too_short[7] = static_cast<std::uint8_t>(without_group >> 8);
    CHECK_FALSE(ParseINSNavGeod(too_short.data(), too_short.size(), m));
}
