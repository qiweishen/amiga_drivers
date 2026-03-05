#ifndef LMS4XXX_SCAN_DATA_PARSER_H
#define LMS4XXX_SCAN_DATA_PARSER_H

#include <cstddef>
#include <cstdint>
#include <system_error>

#include "lms4xxx_scan_data.h"


namespace LMS4xxx {

	// Parser for LMDscandata binary telegrams.
	class ScanDataParser {
	public:
		ScanDataParser() = default;

		// Parse a LMDscandata telegram payload into a ScanData structure.
		[[nodiscard]] static std::error_code Parse(const std::uint8_t *data, std::size_t len, ScanData &out);

	private:
		// Internal cursor for sequential binary field reading.
		struct Reader {
			const std::uint8_t *data;
			std::size_t len;
			std::size_t pos = 0;

			[[nodiscard]] bool has_bytes(std::size_t n) const { return pos + n <= len; }
			[[nodiscard]] std::size_t remaining() const { return len - pos; }

			[[nodiscard]] std::uint8_t read_uint8();
			[[nodiscard]] std::uint16_t read_uint16();
			[[nodiscard]] std::int32_t read_int32();
			[[nodiscard]] std::uint32_t read_uint32();
			[[nodiscard]] float read_float();
			[[nodiscard]] std::string read_string(std::size_t length);
			void skip(std::size_t n);
		};

		// Parse device info block.
		static std::error_code ParseDeviceInfo(Reader &r, ScanData &out);

		// Parse frequency info block.
		static std::error_code ParseFrequency(Reader &r, ScanData &out);

		// Parse encoder block (optional).
		static std::error_code ParseEncoder(Reader &r, ScanData &out);

		// Parse 16-bit channel data blocks.
		static std::error_code ParseChannels16bit(Reader &r, ScanData &out);

		// Parse 8-bit channel data blocks.
		static std::error_code ParseChannels8bit(Reader &r, ScanData &out);

		// Parse position / reserved block.
		static std::error_code ParsePosition(Reader &r, ScanData &out);

		// Parse device name block (optional).
		static std::error_code ParseDeviceName(Reader &r, ScanData &out);

		// Parse timestamp block (optional).
		static std::error_code ParseTimestamp(Reader &r, ScanData &out);

		// Identify 16-bit channel content from 5-byte name string.
		[[nodiscard]] static ChannelContent16 IdentifyChannel16(const std::string &name);

		// Identify 8-bit channel content from 5-byte name string.
		[[nodiscard]] static ChannelContent8 IdentifyChannel8(const std::string &name);
	};

}  // namespace LMS4xxx

#endif	// LMS4XXX_SCAN_DATA_PARSER_H
