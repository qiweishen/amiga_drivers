#include "lms4xxx_scan_data_parser.h"

#include <cstring>

#include "lms4xxx_cola_b.h"
#include "lms4xxx_error.h"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxScanDataParser";

	// Maximum allowed 16-bit channel count (DIST1, RSSI1, REFL1, ANGL1).
	constexpr std::uint16_t kMax16BitChannelCount = 4;

	// Maximum allowed 8-bit channel count (QLTY1 only).
	constexpr std::uint16_t kMax8BitChannelCount = 1;

	// Maximum data points per channel.
	constexpr std::uint16_t kMaxDataPoints = 841;
}  // namespace


namespace LMS4xxx {

	std::uint8_t ScanDataParser::Reader::read_uint8() {
		return data[pos++];
	}

	std::uint16_t ScanDataParser::Reader::read_uint16() {
		const auto val = CoLaBCodec::DecodeUint16(data + pos);
		pos += 2;
		return val;
	}

	std::int32_t ScanDataParser::Reader::read_int32() {
		const auto val = CoLaBCodec::DecodeInt32(data + pos);
		pos += 4;
		return val;
	}

	std::uint32_t ScanDataParser::Reader::read_uint32() {
		const auto val = CoLaBCodec::DecodeUint32(data + pos);
		pos += 4;
		return val;
	}

	float ScanDataParser::Reader::read_float() {
		const auto val = CoLaBCodec::DecodeFloat(data + pos);
		pos += 4;
		return val;
	}

	std::string ScanDataParser::Reader::read_string(std::size_t length) {
		auto val = CoLaBCodec::DecodeString(data + pos, length);
		pos += length;
		return val;
	}

	void ScanDataParser::Reader::skip(std::size_t n) {
		pos += n;
	}

	std::error_code ScanDataParser::Parse(const std::uint8_t *data, std::size_t len, ScanData &out) {
		// Clear output
		out = ScanData{};

		// Minimum payload size check
		if (len < 52) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Payload too short: {} bytes (minimum ~52)", len));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		Reader r{ data, len, 0 };

		// Parse each section in telegram order
		std::error_code ec;

		ec = ParseDeviceInfo(r, out);
		if (ec) {
			return ec;
		}

		ec = ParseFrequency(r, out);
		if (ec) {
			return ec;
		}

		ec = ParseEncoder(r, out);
		if (ec) {
			return ec;
		}

		ec = ParseChannels16bit(r, out);
		if (ec) {
			return ec;
		}

		ec = ParseChannels8bit(r, out);
		if (ec) {
			return ec;
		}

		ec = ParsePosition(r, out);
		if (ec) {
			return ec;
		}

		ec = ParseDeviceName(r, out);
		if (ec) {
			return ec;
		}

		ec = ParseTimestamp(r, out);
		if (ec) {
			return ec;
		}

		// Final reserved field (2 bytes) — optional, may not be present
		if (r.has_bytes(2)) {
			r.skip(2);
		}

		return {};
	}

	std::error_code ScanDataParser::ParseDeviceInfo(Reader &r, ScanData &out) {
		// Version (2B) + Device number (2B) + Serial number (4B) + Device status (2x1B)
		// + Telegram counter (2B) + Scan counter (2B)
		// + Time since startup (4B) + Transmission time (4B)
		// + Digital inputs (2x1B) + Digital outputs (2x1B)
		// + Reserved (2B)
		if (!r.has_bytes(28)) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated device info block, pos={}", r.pos));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		// Version & device info
		out.device_info.version_number = r.read_uint16();
		if (out.device_info.version_number != 1) {
			Common::Log::log_message(spdlog::level::warn, kModule,
									 fmt::format("Unexpected version number: {} (expected 1)", out.device_info.version_number));
		}

		out.device_info.device_number = r.read_uint16();
		out.device_info.serial_number = r.read_uint32();
		out.device_info.device_status_1 = static_cast<DeviceStatus>(r.read_uint8());
		out.device_info.device_status_2 = static_cast<DeviceStatus>(r.read_uint8());

		// Counters
		out.telegram_counter = r.read_uint16();
		out.scan_counter = r.read_uint16();

		// Timing
		out.time_since_startup_us = r.read_uint32();
		out.transmission_time_us = r.read_uint32();

		// Digital I/O
		out.digital_input_1 = r.read_uint8();
		out.digital_input_2 = r.read_uint8();
		out.digital_output_1 = r.read_uint8();
		out.digital_output_2 = r.read_uint8();

		// Reserved (1 × Uint16)
		r.skip(2);

		return {};
	}

	std::error_code ScanDataParser::ParseFrequency(Reader &r, ScanData &out) {
		// Scan frequency (4B) + Measurement frequency (4B)
		if (!r.has_bytes(8)) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated frequency block, pos={}", r.pos));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		out.scan_frequency = r.read_uint32();
		out.measurement_frequency = r.read_uint32();

		return {};
	}

	std::error_code ScanDataParser::ParseEncoder(Reader &r, ScanData &out) {
		// Amount of encoder (2B)
		if (!r.has_bytes(2)) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated encoder count, pos={}", r.pos));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		const auto encoder_count = r.read_uint16();
		out.has_encoder = (encoder_count > 0);

		if (encoder_count > 0) {
			// Encoder position (4B) + Reserved (2B) per encoder
			if (!r.has_bytes(6)) {
				Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated encoder data, pos={}", r.pos));
				return make_error_code(ErrorCode::kFrameTooShort);
			}
			out.encoder.position = r.read_uint32();
			out.encoder.speed = r.read_uint16();  // Reserved field, stored as speed
		}

		return {};
	}

	std::error_code ScanDataParser::ParseChannels16bit(Reader &r, ScanData &out) {
		// Amount of 16-bit channels (2B)
		if (!r.has_bytes(2)) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated 16-bit channel count, pos={}", r.pos));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		const auto channel_count = r.read_uint16();

		if (channel_count > kMax16BitChannelCount) {
			Common::Log::log_message(spdlog::level::warn, kModule,
									 fmt::format("Invalid 16-bit channel count: {} (max {})", channel_count, kMax16BitChannelCount));
			return make_error_code(ErrorCode::kProtocolError);
		}

		out.channels_16bit.reserve(channel_count);

		for (std::uint16_t ch = 0; ch < channel_count; ch++) {
			// Channel header: Content(5B) + Scale(4B) + Offset(4B)
			//                + StartAngle(4B) + AngularStep(2B) + DataCount(2B)
			constexpr std::size_t kChannelHeaderSize = 5 + 4 + 4 + 4 + 2 + 2;
			if (!r.has_bytes(kChannelHeaderSize)) {
				Common::Log::log_message(spdlog::level::warn, kModule,
										 fmt::format("Truncated 16-bit channel {} header, pos={}", ch, r.pos));
				return make_error_code(ErrorCode::kFrameTooShort);
			}

			ChannelData16 channel;

			// Channel name (5 bytes fixed string)
			const auto name = r.read_string(5);
			channel.content = IdentifyChannel16(name);

			channel.scale_factor = r.read_float();
			channel.scale_offset = r.read_float();
			channel.start_angle = r.read_int32();
			channel.angle_step = r.read_uint16();
			channel.num_data = r.read_uint16();

			if (channel.num_data > kMaxDataPoints) {
				Common::Log::log_message(
						spdlog::level::warn, kModule,
						fmt::format("Channel {} data count {} exceeds maximum {}", name, channel.num_data, kMaxDataPoints));
				return make_error_code(ErrorCode::kProtocolError);
			}

			// Read data points (2 bytes each)
			const std::size_t data_bytes = static_cast<std::size_t>(channel.num_data) * 2;
			if (!r.has_bytes(data_bytes)) {
				Common::Log::log_message(
						spdlog::level::warn, kModule,
						fmt::format("Truncated 16-bit channel {} data: need {} bytes, have {}", name, data_bytes, r.remaining()));
				return make_error_code(ErrorCode::kFrameTooShort);
			}

			channel.data.resize(channel.num_data);
			for (std::uint16_t i = 0; i < channel.num_data; i++) {
				channel.data[i] = r.read_uint16();
			}

			out.channels_16bit.push_back(std::move(channel));
		}

		return {};
	}

	std::error_code ScanDataParser::ParseChannels8bit(Reader &r, ScanData &out) {
		// Amount of 8-bit channels (2B)
		if (!r.has_bytes(2)) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated 8-bit channel count, pos={}", r.pos));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		const auto channel_count = r.read_uint16();

		if (channel_count > kMax8BitChannelCount) {
			Common::Log::log_message(spdlog::level::warn, kModule,
									 fmt::format("Invalid 8-bit channel count: {} (max {})", channel_count, kMax8BitChannelCount));
			return make_error_code(ErrorCode::kProtocolError);
		}

		out.channels_8bit.reserve(channel_count);

		for (std::uint16_t ch = 0; ch < channel_count; ch++) {
			// Channel header: Content(5B) + Scale(4B) + Offset(4B)
			//                + StartAngle(4B) + AngularStep(2B) + DataCount(2B)
			constexpr std::size_t kChannelHeaderSize = 5 + 4 + 4 + 4 + 2 + 2;
			if (!r.has_bytes(kChannelHeaderSize)) {
				Common::Log::log_message(spdlog::level::warn, kModule,
										 fmt::format("Truncated 8-bit channel {} header, pos={}", ch, r.pos));
				return make_error_code(ErrorCode::kFrameTooShort);
			}

			ChannelData8 channel;

			const auto name = r.read_string(5);
			channel.content = IdentifyChannel8(name);

			channel.scale_factor = r.read_float();
			channel.scale_offset = r.read_float();
			// Note: 8-bit channel start_angle is documented as Uint32 but
			// we store as Int32 for consistency with 16-bit channels
			channel.start_angle = r.read_int32();
			channel.angle_step = r.read_uint16();
			channel.num_data = r.read_uint16();

			if (channel.num_data > kMaxDataPoints) {
				Common::Log::log_message(
						spdlog::level::warn, kModule,
						fmt::format("Channel {} data count {} exceeds maximum {}", name, channel.num_data, kMaxDataPoints));
				return make_error_code(ErrorCode::kProtocolError);
			}

			// Read data points (1 byte each)
			if (!r.has_bytes(channel.num_data)) {
				Common::Log::log_message(
						spdlog::level::warn, kModule,
						fmt::format("Truncated 8-bit channel {} data: need {} bytes, have {}", name, channel.num_data, r.remaining()));
				return make_error_code(ErrorCode::kFrameTooShort);
			}

			channel.data.resize(channel.num_data);
			for (std::uint16_t i = 0; i < channel.num_data; i++) {
				channel.data[i] = r.read_uint8();
			}

			out.channels_8bit.push_back(std::move(channel));
		}

		return {};
	}

	std::error_code ScanDataParser::ParsePosition(Reader &r, ScanData &out) {
		// Y rotation (4B) — no reserved prefix on LMS4000
		if (!r.has_bytes(4)) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated position block, pos={}", r.pos));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		out.y_rotation = r.read_float();

		return {};
	}

	std::error_code ScanDataParser::ParseDeviceName(Reader &r, ScanData &out) {
		// Name flag (2B), then conditionally: Name length (2B) + Name (var)
		if (!r.has_bytes(2)) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated device name flag, pos={}", r.pos));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		const auto name_flag = r.read_uint16();
		out.has_device_name = (name_flag == 1);

		if (out.has_device_name) {
			if (!r.has_bytes(2)) {
				Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated device name length, pos={}", r.pos));
				return make_error_code(ErrorCode::kFrameTooShort);
			}

			const auto name_len = r.read_uint16();
			if (name_len > 16) {
				Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Device name length {} exceeds max 16", name_len));
				return make_error_code(ErrorCode::kProtocolError);
			}

			if (!r.has_bytes(name_len)) {
				Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated device name string, pos={}", r.pos));
				return make_error_code(ErrorCode::kFrameTooShort);
			}

			out.device_name = r.read_string(name_len);
		}

		return {};
	}

	std::error_code ScanDataParser::ParseTimestamp(Reader &r, ScanData &out) {
		// Time flag (2B), then conditionally:
		//   Year (2B) + Month (1B) + Day (1B) + Hour (1B) + Minute (1B)
		//   + Second (1B) + Microsecond (4B) = 11 bytes
		if (!r.has_bytes(2)) {
			Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated timestamp flag, pos={}", r.pos));
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		const auto time_flag = r.read_uint16();
		out.has_timestamp = (time_flag == 1);

		if (out.has_timestamp) {
			if (!r.has_bytes(11)) {
				Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Truncated timestamp data, pos={}", r.pos));
				return make_error_code(ErrorCode::kFrameTooShort);
			}

			out.timestamp.year = r.read_uint16();
			out.timestamp.month = r.read_uint8();
			out.timestamp.day = r.read_uint8();
			out.timestamp.hour = r.read_uint8();
			out.timestamp.minute = r.read_uint8();
			out.timestamp.second = r.read_uint8();
			out.timestamp.microsecond = r.read_uint32();
		}

		return {};
	}

	ChannelContent16 ScanDataParser::IdentifyChannel16(const std::string &name) {
		if (name == "DIST1")
			return ChannelContent16::kDist1;
		if (name == "RSSI1")
			return ChannelContent16::kRssi1;
		if (name == "REFL1")
			return ChannelContent16::kRefl1;
		if (name == "ANGL1")
			return ChannelContent16::kAngl1;

		Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Unknown 16-bit channel name: '{}'", name));
		return ChannelContent16::kUnknown;
	}

	ChannelContent8 ScanDataParser::IdentifyChannel8(const std::string &name) {
		if (name == "QLTY1")
			return ChannelContent8::kQlty1;

		Common::Log::log_message(spdlog::level::warn, kModule, fmt::format("Unknown 8-bit channel name: '{}'", name));
		return ChannelContent8::kUnknown;
	}

}  // namespace LMS4xxx
