#include "sbf_parsers.hpp"

#include <cstring>

#include "byte_util.h"


namespace asterx::sbf {
	namespace {
		// Bounds-checked little-endian reader over one frame. Unlike the
		// reference (read first, compare iterators after), the bounds check
		// precedes every read, so a truncated frame can never read past the
		// buffer. Floats go through DnuToNan on read — same semantics as the
		// reference qiLittleEndianParser.
		struct Reader {
			const std::uint8_t *p;
			std::size_t size;
			std::size_t pos{ 0 };
			bool ok{ true };

			bool need(std::size_t k) {
				if (ok && size - pos < k) {
					ok = false;
				}
				return ok;
			}
			std::uint8_t u8() {
				if (!need(1)) return 0;
				return p[pos++];
			}
			std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
			std::uint16_t u16() {
				if (!need(2)) return 0;
				const auto v = Common::ByteUtil::LoadLittleU16(p + pos);
				pos += 2;
				return v;
			}
			std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
			std::uint32_t u32() {
				if (!need(4)) return 0;
				const auto v = Common::ByteUtil::LoadLittleU32(p + pos);
				pos += 4;
				return v;
			}
			std::uint64_t u64() {
				if (!need(8)) return 0;
				const auto v = Common::ByteUtil::LoadLittleU64(p + pos);
				pos += 8;
				return v;
			}
			float f32() {
				static_assert(std::numeric_limits<float>::is_iec559);
				const std::uint32_t bits = u32();
				float v;
				std::memcpy(&v, &bits, sizeof v);
				return DnuToNan(v);
			}
			double f64() {
				static_assert(std::numeric_limits<double>::is_iec559);
				const std::uint64_t bits = u64();
				double v;
				std::memcpy(&v, &bits, sizeof v);
				return DnuToNan(v);
			}
			bool skip(std::size_t k) {
				if (!need(k)) return false;
				pos += k;
				return true;
			}
		};

		bool read_header(Reader &r, BlockHeader &h) {
			h.sync_1 = r.u8();
			if (h.sync_1 != 0x24) return false;	 // '$'
			h.sync_2 = r.u8();
			if (h.sync_2 != 0x40) return false;	 // '@'
			h.crc = r.u16();
			const std::uint16_t raw = r.u16();
			h.id = raw & 8191;		  // lower 13 bits
			h.revision = raw >> 13;	  // upper 3 bits
			h.length = r.u16();
			h.tow = r.u32();
			h.wnc = r.u16();
			return r.ok;
		}
	}  // namespace


	std::uint16_t PeekId(const std::uint8_t *data, std::size_t size) {
		if (size < 6) {
			return 0;
		}
		return Common::ByteUtil::LoadLittleU16(data + 4) & 8191;
	}


	bool ParseBlockHeader(const std::uint8_t *data, std::size_t size, BlockHeader &out) {
		Reader r{ data, size };
		return read_header(r, out);
	}


	bool ParsePVTGeodetic(const std::uint8_t *data, std::size_t size, PVTGeodetic &out) {
		Reader r{ data, size };
		if (!read_header(r, out.block_header) || out.block_header.id != kIdPVTGeodetic) {
			return false;
		}
		out.mode = r.u8();
		out.error = r.u8();
		out.latitude = r.f64();
		out.longitude = r.f64();
		out.height = r.f64();
		out.undulation = r.f32();
		out.vn = r.f32();
		out.ve = r.f32();
		out.vu = r.f32();
		out.cog = r.f32();
		out.rx_clk_bias = r.f64();
		out.rx_clk_drift = r.f32();
		out.time_system = r.u8();
		out.datum = r.u8();
		out.nr_sv = r.u8();
		out.wa_corr_info = r.u8();
		out.reference_id = r.u16();
		out.mean_corr_age = r.u16();
		out.signal_info = r.u32();
		out.alert_flag = r.u8();
		if (out.block_header.revision > 0) {
			out.nr_bases = r.u8();
			out.ppp_info = r.u16();
		}
		if (out.block_header.revision > 1) {
			out.latency = r.u16();
			out.h_accuracy = r.u16();
			out.v_accuracy = r.u16();
			out.misc = r.u8();
		}
		// Later revisions may append fields; unread tail bytes are ignored
		return r.ok;
	}


	bool ParseExtSensorMeas(const std::uint8_t *data, std::size_t size, ExtSensorMeas &out) {
		Reader r{ data, size };
		if (!read_header(r, out.block_header) || out.block_header.id != kIdExtSensorMeas) {
			return false;
		}
		out.n = r.u8();
		out.sb_length = r.u8();
		if (!r.ok || out.sb_length != 28) {
			return false;
		}
		// Measurement fields keep their default NaN until the matching
		// sub-block type appears — that is this block's validity contract
		out.source.resize(out.n);
		out.sensor_model.resize(out.n);
		out.type.resize(out.n);
		out.obs_info.resize(out.n);
		bool has_acc = false;
		bool has_omega = false;
		for (std::size_t i = 0; i < out.n; ++i) {
			out.source[i] = r.u8();
			out.sensor_model[i] = r.u8();
			out.type[i] = r.u8();
			out.obs_info[i] = r.u8();
			switch (out.type[i]) {	// each sub-block body is exactly 24 bytes
				case 0:
					out.acceleration_x = r.f64();
					out.acceleration_y = r.f64();
					out.acceleration_z = r.f64();
					has_acc = true;
					break;
				case 1:
					out.angular_rate_x = r.f64();
					out.angular_rate_y = r.f64();
					out.angular_rate_z = r.f64();
					has_omega = true;
					break;
				case 3: {
					const std::int16_t temp = r.i16();
					out.sensor_temperature =
							temp == -32768 ? std::numeric_limits<float>::quiet_NaN() : temp / 100.0f;
					r.skip(22);
					break;
				}
				case 4:
					out.velocity_x = r.f32();
					out.velocity_y = r.f32();
					out.velocity_z = r.f32();
					out.std_dev_x = r.f32();
					out.std_dev_y = r.f32();
					out.std_dev_z = r.f32();
					break;
				case 20:
					out.zero_velocity_flag = r.f64();
					r.skip(16);
					break;
				default:
					r.skip(24);	 // unknown type: skip the body, keep alignment
					break;
			}
		}
		out.has_imu_meas = has_acc && has_omega;
		return r.ok;
	}


	bool ParseINSNavGeod(const std::uint8_t *data, std::size_t size, INSNavGeod &out) {
		Reader r{ data, size };
		if (!read_header(r, out.block_header) ||
			(out.block_header.id != kIdINSNavGeod && out.block_header.id != kIdExtEventINSNavGeod)) {
			return false;
		}
		out.gnss_mode = r.u8();
		out.error = r.u8();
		out.info = r.u16();
		out.gnss_age = r.u16();
		out.latitude = r.f64();
		out.longitude = r.f64();
		out.height = r.f64();
		out.undulation = r.f32();
		out.accuracy = r.u16();
		out.latency = r.u16();
		out.datum = r.u8();
		r.skip(1);	// reserved
		out.sb_list = r.u16();
		// Optional groups appear on the wire in bit order; absent groups keep
		// their default NaN (the reference's setDoNotUse semantics)
		if (out.sb_list & 1) {
			out.latitude_std_dev = r.f32();
			out.longitude_std_dev = r.f32();
			out.height_std_dev = r.f32();
		}
		if (out.sb_list & 2) {
			out.heading = r.f32();
			out.pitch = r.f32();
			out.roll = r.f32();
		}
		if (out.sb_list & 4) {
			out.heading_std_dev = r.f32();
			out.pitch_std_dev = r.f32();
			out.roll_std_dev = r.f32();
		}
		if (out.sb_list & 8) {
			out.ve = r.f32();
			out.vn = r.f32();
			out.vu = r.f32();
		}
		if (out.sb_list & 16) {
			out.ve_std_dev = r.f32();
			out.vn_std_dev = r.f32();
			out.vu_std_dev = r.f32();
		}
		if (out.sb_list & 32) {
			out.latitude_longitude_cov = r.f32();
			out.latitude_height_cov = r.f32();
			out.longitude_height_cov = r.f32();
		}
		if (out.sb_list & 64) {
			out.heading_pitch_cov = r.f32();
			out.heading_roll_cov = r.f32();
			out.pitch_roll_cov = r.f32();
		}
		if (out.sb_list & 128) {
			out.ve_vn_cov = r.f32();
			out.ve_vu_cov = r.f32();
			out.vn_vu_cov = r.f32();
		}
		return r.ok;
	}


	bool ParseReceiverStatus(const std::uint8_t *data, std::size_t size, ReceiverStatus &out) {
		Reader r{ data, size };
		if (!read_header(r, out.block_header) || out.block_header.id != kIdReceiverStatus) {
			return false;
		}
		out.cpu_load = r.u8();
		out.ext_error = r.u8();
		out.up_time = r.u32();
		out.rx_status = r.u32();
		out.rx_error = r.u32();
		out.n = r.u8();
		if (!r.ok || out.n > 18) {	// MAXSB_AGCSTATE
			return false;
		}
		out.sb_length = r.u8();
		out.cmd_count = r.u8();
		out.temperature = r.u8();
		if (out.n > 0 && out.sb_length < 4) {
			// Not in the reference: a sub-4 sb_length would mean a negative
			// padding skip there; reject the frame instead
			return false;
		}
		out.agc_state.resize(out.n);
		for (auto &agc : out.agc_state) {
			// The reference AgcStateParser takes its iterator BY VALUE
			// (sbf_blocks.hpp:1424), so every sub-block reads the same bytes
			// and the outer iterator never advances — fixed here
			agc.frontend_id = r.u8();
			agc.gain = r.i8();
			agc.sample_var = r.u8();
			agc.blanking_stat = r.u8();
			r.skip(out.sb_length - 4);	// padding
		}
		return r.ok;
	}


	bool ParseAttEuler(const std::uint8_t *data, std::size_t size, AttEuler &out) {
		Reader r{ data, size };
		if (!read_header(r, out.block_header) || out.block_header.id != kIdAttEuler) {
			return false;
		}
		out.nr_sv = r.u8();
		out.error = r.u8();
		out.mode = r.u16();
		r.skip(2);	// reserved
		out.heading = r.f32();
		out.pitch = r.f32();
		out.roll = r.f32();
		out.pitch_dot = r.f32();
		out.roll_dot = r.f32();
		out.heading_dot = r.f32();
		return r.ok;
	}
}  // namespace asterx::sbf
