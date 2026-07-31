// Synthetic-frame tests for the live SBF parsers. Frames are hand-built in
// wire order (little endian); the CRC field is a placeholder — ssnrx validates
// CRC before the parsers ever see a frame, so they do not re-check it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "sbf_parsers.hpp"

using namespace asterx::sbf;

namespace {
	struct FrameBuilder {
		std::vector<std::uint8_t> b; // body only; the header is prepended by finalize()

		void u8(std::uint8_t v) { b.push_back(v); }
		void i8(std::int8_t v) { b.push_back(static_cast<std::uint8_t>(v)); }
		void u16(std::uint16_t v) {
			b.push_back(v & 0xFF);
			b.push_back(v >> 8);
		}
		void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
		void u32(std::uint32_t v) {
			u16(v & 0xFFFF);
			u16(v >> 16);
		}
		void f32(float v) {
			std::uint32_t bits;
			std::memcpy(&bits, &v, sizeof bits);
			u32(bits);
		}
		void f64(double v) {
			std::uint64_t bits;
			std::memcpy(&bits, &v, sizeof bits);
			u32(static_cast<std::uint32_t>(bits & 0xFFFFFFFFull));
			u32(static_cast<std::uint32_t>(bits >> 32));
		}

		std::vector<std::uint8_t> finalize(std::uint16_t id, std::uint8_t rev,
										   std::uint32_t tow = 123456, std::uint16_t wnc = 2400) const {
			std::vector<std::uint8_t> f;
			std::size_t total = 14 + b.size();
			total = (total + 3) / 4 * 4; // SBF frames are padded to a multiple of 4
			f.push_back(0x24); // '$'
			f.push_back(0x40); // '@'
			f.push_back(0xEF); // crc placeholder (not checked by the parsers)
			f.push_back(0xBE);
			const std::uint16_t raw = static_cast<std::uint16_t>(id | (rev << 13));
			f.push_back(raw & 0xFF);
			f.push_back(raw >> 8);
			f.push_back(total & 0xFF);
			f.push_back(static_cast<std::uint8_t>(total >> 8));
			f.push_back(tow & 0xFF);
			f.push_back((tow >> 8) & 0xFF);
			f.push_back((tow >> 16) & 0xFF);
			f.push_back((tow >> 24) & 0xFF);
			f.push_back(wnc & 0xFF);
			f.push_back(wnc >> 8);
			f.insert(f.end(), b.begin(), b.end());
			f.resize(total, 0); // padding
			return f;
		}
	};

	// PVT body up to alert_flag (revision-0 extent)
	void pvt_rev0_body(FrameBuilder &fb) {
		fb.u8(4);            // mode: RTK Fixed
		fb.u8(0);            // error
		fb.f64(0.6);         // latitude rad
		fb.f64(2.1);         // longitude rad
		fb.f64(15.25);       // height
		fb.f32(9.0f);        // undulation
		fb.f32(0.5f);        // vn
		fb.f32(-0.25f);      // ve
		fb.f32(0.125f);      // vu
		fb.f32(123.5f);      // cog
		fb.f64(0.001);       // rx_clk_bias
		fb.f32(0.01f);       // rx_clk_drift
		fb.u8(0);            // time_system
		fb.u8(0);            // datum
		fb.u8(23);           // nr_sv
		fb.u8(0);            // wa_corr_info
		fb.u16(0);           // reference_id
		fb.u16(150);         // mean_corr_age (1.5 s)
		fb.u32(0x5);         // signal_info
		fb.u8(0);            // alert_flag
	}
}


TEST(BlockHeader, Unpack) {
	FrameBuilder fb;
	const auto f = fb.finalize(4007, 2, 987654, 2345);
	BlockHeader h;
	ASSERT_TRUE(ParseBlockHeader(f.data(), f.size(), h));
	EXPECT_EQ(h.id, 4007);
	EXPECT_EQ(h.revision, 2);
	EXPECT_EQ(h.tow, 987654u);
	EXPECT_EQ(h.wnc, 2345);
	EXPECT_EQ(PeekId(f.data(), f.size()), 4007);

	auto bad = f;
	bad[0] = 0x25; // wrong sync byte 1
	EXPECT_FALSE(ParseBlockHeader(bad.data(), bad.size(), h));
}


TEST(Pvt, Rev2Full) {
	FrameBuilder fb;
	pvt_rev0_body(fb);
	fb.u8(2);      // nr_bases       (rev > 0)
	fb.u16(60);    // ppp_info
	fb.u16(500);   // latency        (rev > 1, 0.05 s)
	fb.u16(12);    // h_accuracy     (0.12 m)
	fb.u16(18);    // v_accuracy     (0.18 m)
	fb.u8(1);      // misc
	const auto f = fb.finalize(4007, 2);
	EXPECT_EQ(f.size(), 96u); // 14 + 81 body, padded to 96 (firmware manual layout)

	PVTGeodetic m;
	ASSERT_TRUE(ParsePVTGeodetic(f.data(), f.size(), m));
	EXPECT_EQ(m.mode, 4);
	EXPECT_EQ(m.error, 0);
	EXPECT_DOUBLE_EQ(m.latitude, 0.6);
	EXPECT_DOUBLE_EQ(m.longitude, 2.1);
	EXPECT_DOUBLE_EQ(m.height, 15.25);
	EXPECT_FLOAT_EQ(m.undulation, 9.0f);
	EXPECT_FLOAT_EQ(m.vn, 0.5f);
	EXPECT_FLOAT_EQ(m.ve, -0.25f);
	EXPECT_FLOAT_EQ(m.cog, 123.5f);
	EXPECT_EQ(m.nr_sv, 23);
	EXPECT_EQ(m.mean_corr_age, 150);
	EXPECT_EQ(m.signal_info, 0x5u);
	EXPECT_EQ(m.nr_bases, 2);
	EXPECT_EQ(m.ppp_info, 60);
	EXPECT_EQ(m.latency, 500);
	EXPECT_EQ(m.h_accuracy, 12);
	EXPECT_EQ(m.v_accuracy, 18);
	EXPECT_EQ(m.misc, 1);
}


TEST(Pvt, DnuSentinels) {
	FrameBuilder fb;
	fb.u8(0);
	fb.u8(1);            // error != 0
	fb.f64(-2e10);       // latitude DNU
	fb.f64(-2e10);       // longitude DNU
	fb.f64(-2e10);       // height DNU
	fb.f32(-2e10f);      // undulation DNU
	fb.f32(-2e10f);      // vn
	fb.f32(-2e10f);      // ve
	fb.f32(-2e10f);      // vu
	fb.f32(-2e10f);      // cog
	fb.f64(-2e10);       // rx_clk_bias
	fb.f32(-2e10f);      // rx_clk_drift
	fb.u8(0);
	fb.u8(0);
	fb.u8(255);          // nr_sv DNU
	fb.u8(0);
	fb.u16(65535);       // reference_id DNU
	fb.u16(65535);       // mean_corr_age DNU
	fb.u32(0);
	fb.u8(0);
	const auto f = fb.finalize(4007, 0);

	PVTGeodetic m;
	ASSERT_TRUE(ParsePVTGeodetic(f.data(), f.size(), m));
	EXPECT_TRUE(std::isnan(m.latitude));
	EXPECT_TRUE(std::isnan(m.longitude));
	EXPECT_TRUE(std::isnan(m.undulation));
	EXPECT_TRUE(std::isnan(m.cog));
	EXPECT_EQ(m.nr_sv, 255);
	EXPECT_EQ(m.mean_corr_age, 65535);
	EXPECT_FALSE(Valid(m.mean_corr_age));
}


TEST(Pvt, Rev0KeepsDnuDefaults) {
	FrameBuilder fb;
	pvt_rev0_body(fb);
	const auto f = fb.finalize(4007, 0);

	PVTGeodetic m;
	ASSERT_TRUE(ParsePVTGeodetic(f.data(), f.size(), m));
	EXPECT_EQ(m.nr_sv, 23);
	// Fields a revision-0 frame does not carry keep their DNU defaults
	EXPECT_EQ(m.latency, 65535);
	EXPECT_EQ(m.h_accuracy, 65535);
	EXPECT_EQ(m.v_accuracy, 65535);
	EXPECT_EQ(m.nr_bases, 0);
}


TEST(Pvt, Rev3ForwardCompatible) {
	FrameBuilder fb;
	pvt_rev0_body(fb);
	fb.u8(2);
	fb.u16(60);
	fb.u16(500);
	fb.u16(12);
	fb.u16(18);
	fb.u8(1);
	for (int i = 0; i < 8; ++i) fb.u8(0xAA); // unknown future fields
	const auto f = fb.finalize(4007, 3);

	PVTGeodetic m;
	ASSERT_TRUE(ParsePVTGeodetic(f.data(), f.size(), m));
	EXPECT_EQ(m.h_accuracy, 12); // known fields parsed, tail ignored
}


TEST(Pvt, TruncatedFrameRejected) {
	FrameBuilder fb;
	pvt_rev0_body(fb);
	auto f = fb.finalize(4007, 2);
	f.resize(50); // cut mid-body; must fail without reading past the buffer
	PVTGeodetic m;
	EXPECT_FALSE(ParsePVTGeodetic(f.data(), f.size(), m));
}


TEST(Pvt, WrongIdRejected) {
	FrameBuilder fb;
	pvt_rev0_body(fb);
	const auto f = fb.finalize(4014, 0);
	PVTGeodetic m;
	EXPECT_FALSE(ParsePVTGeodetic(f.data(), f.size(), m));
}


TEST(Ext, AccPlusGyro) {
	FrameBuilder fb;
	fb.u8(2);   // n
	fb.u8(28);  // sb_length
	// sub-block 1: acceleration
	fb.u8(0); fb.u8(2); fb.u8(0); fb.u8(0); // source, model, type=0, obs_info
	fb.f64(1.0); fb.f64(2.0); fb.f64(3.0);
	// sub-block 2: angular rate
	fb.u8(0); fb.u8(2); fb.u8(1); fb.u8(0);
	fb.f64(4.0); fb.f64(5.0); fb.f64(6.0);
	const auto f = fb.finalize(4050, 0);

	ExtSensorMeas m;
	ASSERT_TRUE(ParseExtSensorMeas(f.data(), f.size(), m));
	EXPECT_TRUE(m.has_imu_meas);
	EXPECT_DOUBLE_EQ(m.acceleration_x, 1.0);
	EXPECT_DOUBLE_EQ(m.acceleration_z, 3.0);
	EXPECT_DOUBLE_EQ(m.angular_rate_x, 4.0);
	EXPECT_DOUBLE_EQ(m.angular_rate_z, 6.0);
	EXPECT_TRUE(std::isnan(m.velocity_x));
	EXPECT_TRUE(std::isnan(m.sensor_temperature));
}


TEST(Ext, UnknownTypeSkippedKeepsAlignment) {
	FrameBuilder fb;
	fb.u8(2);
	fb.u8(28);
	// sub-block 1: unknown type 99, 24 bytes of garbage
	fb.u8(0); fb.u8(2); fb.u8(99); fb.u8(0);
	for (int i = 0; i < 24; ++i) fb.u8(0xAA);
	// sub-block 2: angular rate — must still parse correctly after the skip
	fb.u8(0); fb.u8(2); fb.u8(1); fb.u8(0);
	fb.f64(4.0); fb.f64(5.0); fb.f64(6.0);
	const auto f = fb.finalize(4050, 0);

	ExtSensorMeas m;
	ASSERT_TRUE(ParseExtSensorMeas(f.data(), f.size(), m));
	EXPECT_FALSE(m.has_imu_meas); // no acceleration sub-block
	EXPECT_DOUBLE_EQ(m.angular_rate_y, 5.0);
	EXPECT_TRUE(std::isnan(m.acceleration_x));
}


TEST(Ext, TemperatureSentinelAndValue) {
	FrameBuilder fb;
	fb.u8(1);
	fb.u8(28);
	fb.u8(0); fb.u8(2); fb.u8(3); fb.u8(0);
	fb.i16(-32768); // sentinel
	for (int i = 0; i < 22; ++i) fb.u8(0);
	const auto f1 = fb.finalize(4050, 0);
	ExtSensorMeas m1;
	ASSERT_TRUE(ParseExtSensorMeas(f1.data(), f1.size(), m1));
	EXPECT_TRUE(std::isnan(m1.sensor_temperature));

	FrameBuilder fb2;
	fb2.u8(1);
	fb2.u8(28);
	fb2.u8(0); fb2.u8(2); fb2.u8(3); fb2.u8(0);
	fb2.i16(2534); // 25.34 degC
	for (int i = 0; i < 22; ++i) fb2.u8(0);
	const auto f2 = fb2.finalize(4050, 0);
	ExtSensorMeas m2;
	ASSERT_TRUE(ParseExtSensorMeas(f2.data(), f2.size(), m2));
	EXPECT_FLOAT_EQ(m2.sensor_temperature, 25.34f);
}


TEST(Ext, BadSbLengthRejected) {
	FrameBuilder fb;
	fb.u8(1);
	fb.u8(24); // must be 28
	const auto f = fb.finalize(4050, 0);
	ExtSensorMeas m;
	EXPECT_FALSE(ParseExtSensorMeas(f.data(), f.size(), m));
}


namespace {
	// INSNavGeod fixed part (up to sb_list, exclusive)
	void ins_fixed_body(FrameBuilder &fb) {
		fb.u8(4);       // gnss_mode
		fb.u8(0);       // error
		fb.u16(0);      // info
		fb.u16(5);      // gnss_age (0.05 s)
		fb.f64(0.55);   // latitude rad
		fb.f64(2.12);   // longitude rad
		fb.f64(16.5);   // height
		fb.f32(9.1f);   // undulation
		fb.u16(8);      // accuracy (0.08 m)
		fb.u16(30);     // latency
		fb.u8(0);       // datum
		fb.u8(0);       // reserved
	}
}


TEST(Ins, SbListEmpty) {
	FrameBuilder fb;
	ins_fixed_body(fb);
	fb.u16(0); // sb_list
	const auto f = fb.finalize(4226, 0);
	INSNavGeod m;
	ASSERT_TRUE(ParseINSNavGeod(f.data(), f.size(), m));
	EXPECT_DOUBLE_EQ(m.latitude, 0.55);
	EXPECT_TRUE(std::isnan(m.heading));
	EXPECT_TRUE(std::isnan(m.ve));
	EXPECT_TRUE(std::isnan(m.latitude_std_dev));
	EXPECT_TRUE(std::isnan(m.vn_vu_cov));
}


TEST(Ins, SbListPartialWireOrderIsBitOrder) {
	// bit1 (attitude) + bit3 (velocity): the wire carries attitude FIRST, then
	// velocity — regression for the reference .msg declaration-order trap
	FrameBuilder fb;
	ins_fixed_body(fb);
	fb.u16(0x0A); // bits 1 and 3
	fb.f32(90.5f); fb.f32(1.25f); fb.f32(-0.5f); // heading, pitch, roll
	fb.f32(0.5f); fb.f32(0.25f); fb.f32(-0.125f); // ve, vn, vu
	const auto f = fb.finalize(4226, 0);
	INSNavGeod m;
	ASSERT_TRUE(ParseINSNavGeod(f.data(), f.size(), m));
	EXPECT_FLOAT_EQ(m.heading, 90.5f);
	EXPECT_FLOAT_EQ(m.roll, -0.5f);
	EXPECT_FLOAT_EQ(m.ve, 0.5f);
	EXPECT_FLOAT_EQ(m.vu, -0.125f);
	EXPECT_TRUE(std::isnan(m.heading_std_dev)); // absent groups stay NaN
	EXPECT_TRUE(std::isnan(m.latitude_std_dev));
}


TEST(Ins, SbListFullAndEventId) {
	FrameBuilder fb;
	ins_fixed_body(fb);
	fb.u16(0xFF); // all eight groups
	for (int i = 0; i < 24; ++i) fb.f32(static_cast<float>(i) + 0.5f);
	const auto f = fb.finalize(4230, 0); // ExtEventINSNavGeod shares the layout
	INSNavGeod m;
	ASSERT_TRUE(ParseINSNavGeod(f.data(), f.size(), m));
	EXPECT_FLOAT_EQ(m.latitude_std_dev, 0.5f);   // group bit0, first value
	EXPECT_FLOAT_EQ(m.heading, 3.5f);            // group bit1 starts at index 3
	EXPECT_FLOAT_EQ(m.vn_vu_cov, 23.5f);         // last value of group bit7
}


TEST(Rxs, TwoAgcSubBlocksAreDistinct) {
	// Regression for the reference AgcStateParser taking its iterator by value
	// (every AGC sub-block read the same bytes there)
	FrameBuilder fb;
	fb.u8(35);      // cpu_load
	fb.u8(0);       // ext_error
	fb.u32(3600);   // up_time
	fb.u32(0);      // rx_status
	fb.u32(0);      // rx_error
	fb.u8(2);       // n
	fb.u8(8);       // sb_length (4 data + 4 padding)
	fb.u8(1);       // cmd_count
	fb.u8(142);     // temperature (42 degC)
	fb.u8(1); fb.i8(50); fb.u8(10); fb.u8(0);  // AGC 1
	fb.u8(0); fb.u8(0); fb.u8(0); fb.u8(0);    // padding
	fb.u8(2); fb.i8(-3); fb.u8(20); fb.u8(5);  // AGC 2
	fb.u8(0); fb.u8(0); fb.u8(0); fb.u8(0);    // padding
	const auto f = fb.finalize(4014, 0);

	ReceiverStatus m;
	ASSERT_TRUE(ParseReceiverStatus(f.data(), f.size(), m));
	EXPECT_EQ(m.cpu_load, 35);
	EXPECT_EQ(m.temperature, 142);
	ASSERT_EQ(m.agc_state.size(), 2u);
	EXPECT_EQ(m.agc_state[0].frontend_id, 1);
	EXPECT_EQ(m.agc_state[0].gain, 50);
	EXPECT_EQ(m.agc_state[1].frontend_id, 2);
	EXPECT_EQ(m.agc_state[1].gain, -3);
	EXPECT_NE(m.agc_state[0].frontend_id, m.agc_state[1].frontend_id);
}


TEST(Rxs, TooManyAgcRejected) {
	FrameBuilder fb;
	fb.u8(0); fb.u8(0); fb.u32(0); fb.u32(0); fb.u32(0);
	fb.u8(19); // n > 18
	fb.u8(8); fb.u8(0); fb.u8(100);
	const auto f = fb.finalize(4014, 0);
	ReceiverStatus m;
	EXPECT_FALSE(ParseReceiverStatus(f.data(), f.size(), m));
}


TEST(Rxs, BadSbLengthRejected) {
	FrameBuilder fb;
	fb.u8(0); fb.u8(0); fb.u32(0); fb.u32(0); fb.u32(0);
	fb.u8(1);
	fb.u8(2); // sb_length < 4 would need a negative padding skip
	fb.u8(0); fb.u8(100);
	fb.u8(1); fb.u8(2); // (truncated garbage)
	const auto f = fb.finalize(4014, 0);
	ReceiverStatus m;
	EXPECT_FALSE(ParseReceiverStatus(f.data(), f.size(), m));
}


TEST(Att, NormalAndDnu) {
	FrameBuilder fb;
	fb.u8(12);      // nr_sv
	fb.u8(0);       // error
	fb.u16(2);      // mode
	fb.u16(0);      // reserved
	fb.f32(181.5f); // heading
	fb.f32(1.25f);  // pitch
	fb.f32(-0.75f); // roll
	fb.f32(0.1f);   // pitch_dot
	fb.f32(0.2f);   // roll_dot
	fb.f32(0.3f);   // heading_dot
	const auto f = fb.finalize(5938, 0);
	AttEuler m;
	ASSERT_TRUE(ParseAttEuler(f.data(), f.size(), m));
	EXPECT_EQ(m.nr_sv, 12);
	EXPECT_FLOAT_EQ(m.heading, 181.5f);
	EXPECT_FLOAT_EQ(m.roll, -0.75f);
	EXPECT_FLOAT_EQ(m.heading_dot, 0.3f);

	FrameBuilder fb2;
	fb2.u8(255); fb2.u8(1); fb2.u16(0); fb2.u16(0);
	for (int i = 0; i < 6; ++i) fb2.f32(-2e10f); // all DNU
	const auto f2 = fb2.finalize(5938, 0);
	AttEuler m2;
	ASSERT_TRUE(ParseAttEuler(f2.data(), f2.size(), m2));
	EXPECT_TRUE(std::isnan(m2.heading));
	EXPECT_TRUE(std::isnan(m2.heading_dot));
	EXPECT_EQ(m2.nr_sv, 255);

	// Frame with a different block id must be rejected
	AttEuler m3;
	const auto fp = fb.finalize(4007, 0);
	EXPECT_FALSE(ParseAttEuler(fp.data(), fp.size(), m3));
}


TEST(Time, GpsToUnixNs) {
	// tow=0, wnc=0 -> GPS epoch minus leap seconds
	EXPECT_EQ(GpsToUnixNs(0, 0), 315964800ull * 1000000000ull - 18ull * 1000000000ull);
	// one week + 1.5 s into week 1
	EXPECT_EQ(GpsToUnixNs(1500, 1),
			  315964800ull * 1000000000ull + 604800ull * 1000000000ull + 1500000000ull - 18000000000ull);
	// DNU -> 0 (caller falls back to the host timestamp)
	EXPECT_EQ(GpsToUnixNs(4294967295u, 100), 0u);
	EXPECT_EQ(GpsToUnixNs(1000, 65535), 0u);
}
