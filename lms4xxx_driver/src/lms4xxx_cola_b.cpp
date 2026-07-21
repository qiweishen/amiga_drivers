#include "lms4xxx_cola_b.h"

#include "byte_util.h"

#include <cstring>

#include "lms4xxx_error.h"
#include "logger.h"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxCoLaBCodec";
	Common::DriverLog g_log{ std::string(kModule) };
}  // namespace


namespace LMS4xxx {

	std::vector<std::uint8_t> CoLaBCodec::Encode(std::string_view command_type, std::string_view command_name,
												 const std::vector<std::uint8_t> &params) {
		// Build data section: CommandType + 0x20 + CommandName [+ 0x20 + Params]
		std::vector<std::uint8_t> data;
		data.reserve(command_type.size() + 1 + command_name.size() + (params.empty() ? 0 : 1 + params.size()));

		// Command type (3 bytes)
		data.insert(data.end(), command_type.begin(), command_type.end());
		data.push_back(kCoLaBSpace);
		data.insert(data.end(), command_name.begin(), command_name.end());

		// Parameters (preceded by space if present)
		if (!params.empty()) {
			data.push_back(kCoLaBSpace);
			data.insert(data.end(), params.begin(), params.end());
		}

		// Compute frame: STX(4) + Length(4) + Data + Checksum(1)
		const auto data_len = static_cast<std::uint32_t>(data.size());
		const auto len_bytes = EncodeUint32(data_len);
		const auto checksum = ComputeChecksum(data.data(), data.size());

		std::vector<std::uint8_t> frame;
		frame.reserve(4 + 4 + data.size() + 1);

		frame.insert(frame.end(), kCoLaBStx.begin(), kCoLaBStx.end());
		frame.insert(frame.end(), len_bytes.begin(), len_bytes.end());
		frame.insert(frame.end(), data.begin(), data.end());
		frame.push_back(checksum);

		return frame;
	}


	std::array<std::uint8_t, 4> CoLaBCodec::EncodeUint32(std::uint32_t value) {
		std::array<std::uint8_t, 4> out{};
		Common::ByteUtil::StoreBigU32(out.data(), value);
		return out;
	}


	std::array<std::uint8_t, 2> CoLaBCodec::EncodeUint16(std::uint16_t value) {
		return { {
				static_cast<std::uint8_t>((value >> 8) & 0xFF),
				static_cast<std::uint8_t>(value & 0xFF),
		} };
	}


	std::array<std::uint8_t, 4> CoLaBCodec::EncodeInt32(std::int32_t value) {
		return EncodeUint32(static_cast<std::uint32_t>(value));
	}


	std::array<std::uint8_t, 4> CoLaBCodec::EncodeFloat(float value) {
		std::uint32_t raw;
		std::memcpy(&raw, &value, sizeof(float));
		return EncodeUint32(raw);
	}


	std::error_code CoLaBCodec::Decode(const std::uint8_t *data, std::size_t len, CoLaBMessage &msg) {
		// Minimum: CommandType(3) + Space(1) = 4 bytes
		if (len < 4) {
			g_log.warn("Frame data too short: {} bytes", len);
			return make_error_code(ErrorCode::kFrameTooShort);
		}

		std::size_t pos = 0;

		// Extract command type (3 bytes)
		msg.command_type.assign(reinterpret_cast<const char *>(data + pos), 3);
		pos += 3;

		// Expect space separator
		if (data[pos] != kCoLaBSpace) {
			g_log.warn("Expected space after command type, got 0x{:02X}", data[pos]);
			return make_error_code(ErrorCode::kProtocolError);
		}
		pos++;

		// Extract command name: read until next space or end of data
		std::size_t name_start = pos;
		while (pos < len && data[pos] != kCoLaBSpace) {
			pos++;
		}

		if (pos == name_start) {
			g_log.warn("Empty command name");
			return make_error_code(ErrorCode::kProtocolError);
		}

		msg.command_name.assign(reinterpret_cast<const char *>(data + name_start), pos - name_start);

		// Remaining bytes are payload (skip leading space if present)
		if (pos < len && data[pos] == kCoLaBSpace) {
			pos++;
		}

		if (pos < len) {
			msg.payload.assign(data + pos, data + len);
		} else {
			msg.payload.clear();
		}

		return {};
	}


	std::uint32_t CoLaBCodec::DecodeUint32(const std::uint8_t *buf) {
		return Common::ByteUtil::LoadBigU32(buf);
	}


	std::uint16_t CoLaBCodec::DecodeUint16(const std::uint8_t *buf) {
		return Common::ByteUtil::LoadBigU16(buf);
	}


	std::int32_t CoLaBCodec::DecodeInt32(const std::uint8_t *buf) {
		return static_cast<std::int32_t>(DecodeUint32(buf));
	}


	float CoLaBCodec::DecodeFloat(const std::uint8_t *buf) {
		// IEEE 754 single-precision, big-endian -> host
		const std::uint32_t raw = DecodeUint32(buf);
		float result;
		std::memcpy(&result, &raw, sizeof(float));
		return result;
	}


	std::string CoLaBCodec::DecodeString(const std::uint8_t *buf, std::size_t len) {
		return std::string(reinterpret_cast<const char *>(buf), len);
	}


	std::uint8_t CoLaBCodec::ComputeChecksum(const std::uint8_t *data, std::size_t len) {
		std::uint8_t cs = 0;
		for (std::size_t i = 0; i < len; i++) {
			cs ^= data[i];
		}
		return cs;
	}

}  // namespace LMS4xxx
