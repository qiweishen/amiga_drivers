#include "lms4xxx_frame_receiver.h"

#include <chrono>
#include <cstring>

#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxFrameReceiver";

	// Size of the STX header (4 x 0x02).
	constexpr std::size_t kStxSize = 4;

	// Size of the header: STX(4) + Length(4).
	constexpr std::size_t kHeaderSize = kStxSize + 4;

	// Read a big-endian uint32 from a byte buffer.
	inline std::uint32_t read_uint32_be(const std::uint8_t *p) {
		return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
			   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
	}

	// Get current timestamp in microseconds (monotonic clock).
	inline std::uint64_t now_us() {
		using clock = std::chrono::steady_clock;
		auto now = clock::now();
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
	}
}  // namespace


namespace LMS4xxx {

	struct FrameReceiver::Impl {
		FrameCallback on_frame;
		ErrorCallback on_error;
		std::size_t max_frame_size;

		// Linear accumulation buffer. Pre-allocated to hold at least 2 max frames.
		std::vector<std::uint8_t> buffer;
		std::size_t write_pos = 0;

		Impl(FrameCallback on_frame_cb, ErrorCallback on_error_cb, std::size_t max_frame) :
			on_frame(std::move(on_frame_cb)), on_error(std::move(on_error_cb)), max_frame_size(max_frame) {
			// Pre-allocate buffer for 2x max frame size to avoid reallocation.
			buffer.resize(max_frame_size * 2 + kHeaderSize + 1);
		}

		// Search for STX marker in the accumulated buffer starting at `offset`.
		// Returns the position of the first byte of STX, or `write_pos` if not found.
		std::size_t find_stx(std::size_t offset) const {
			if (write_pos < offset + kStxSize) {
				return write_pos;
			}
			// Search for four consecutive 0x02 bytes.
			const std::size_t search_end = write_pos - kStxSize + 1;
			for (std::size_t i = offset; i < search_end; ++i) {
				if (buffer[i] == 0x02 && buffer[i + 1] == 0x02 && buffer[i + 2] == 0x02 && buffer[i + 3] == 0x02) {
					return i;
				}
			}
			return write_pos;
		}

		// Compact the buffer by removing consumed bytes [0, offset).
		void compact(std::size_t offset) {
			if (offset == 0) {
				return;
			}
			if (offset >= write_pos) {
				write_pos = 0;
				return;
			}
			std::size_t remaining = write_pos - offset;
			std::memmove(buffer.data(), buffer.data() + offset, remaining);
			write_pos = remaining;
		}

		// Try to extract one complete frame starting at `pos`.
		// Returns the number of bytes consumed (0 if incomplete / need more data).
		std::size_t try_extract_frame(std::size_t pos) {
			std::size_t available = write_pos - pos;

			// Need at least the header to determine frame length.
			if (available < kHeaderSize) {
				return 0;
			}

			// Read the data length field (big-endian).
			std::uint32_t data_len = read_uint32_be(buffer.data() + pos + kStxSize);

			// Sanity check: data_len must not exceed max_frame_size minus overhead.
			if (data_len > max_frame_size) {
				if (on_error) {
					on_error("frame data length exceeds maximum");
				}
				Common::Log::log_message(spdlog::level::warn, kModule,
										 fmt::format("Frame data length {} exceeds max {}, skipping STX", data_len, max_frame_size));
				// Skip past this STX marker and try to resync.
				return kStxSize;
			}

			// Total frame size: STX(4) + Length(4) + Data(data_len) + Checksum(1).
			std::size_t total_frame_size = kHeaderSize + data_len + 1;

			// Do we have the complete frame?
			if (available < total_frame_size) {
				return 0;  // Need more data.
			}

			// Pointers to frame components.
			const std::uint8_t *data_start = buffer.data() + pos + kHeaderSize;
			std::uint8_t received_cs = buffer[pos + kHeaderSize + data_len];

			// Validate CRC8 (XOR over data portion).
			std::uint8_t computed_cs = compute_crc8_internal(data_start, data_len);

			if (computed_cs != received_cs) {
				if (on_error) {
					on_error("CRC8 checksum mismatch");
				}
				Common::Log::log_message(spdlog::level::warn, kModule,
										 fmt::format("CRC8 mismatch: computed 0x{:02X}, received 0x{:02X}", computed_cs, received_cs));
				// Skip past this STX and try to resync.
				return kStxSize;
			}

			// Frame is valid — construct RawFrame and deliver.
			RawFrame frame;
			frame.data.assign(data_start, data_start + data_len);
			frame.receive_timestamp_us = now_us();

			if (on_frame) {
				on_frame(std::move(frame));
			}

			return total_frame_size;
		}

		// Static CRC8 computation (XOR over all bytes).
		static std::uint8_t compute_crc8_internal(const std::uint8_t *data, std::size_t len) {
			std::uint8_t cs = 0;
			for (std::size_t i = 0; i < len; ++i) {
				cs ^= data[i];
			}
			return cs;
		}
	};


	FrameReceiver::FrameReceiver(FrameCallback on_frame, ErrorCallback on_error, std::size_t max_frame_size) :
		impl_(std::make_unique<Impl>(std::move(on_frame), std::move(on_error), max_frame_size)) {}


	FrameReceiver::~FrameReceiver() = default;


	FrameReceiver::FrameReceiver(FrameReceiver &&) noexcept = default;
	FrameReceiver &FrameReceiver::operator=(FrameReceiver &&) noexcept = default;


	void FrameReceiver::Feed(const std::uint8_t *data, std::size_t len) {
		if (len == 0) {
			return;
		}

		// Grow buffer if needed (should not happen with proper pre-allocation).
		if (impl_->write_pos + len > impl_->buffer.size()) {
			impl_->buffer.resize(impl_->write_pos + len + impl_->max_frame_size);
		}

		// Append incoming data.
		std::memcpy(impl_->buffer.data() + impl_->write_pos, data, len);
		impl_->write_pos += len;

		// Process all complete frames in the buffer.
		std::size_t scan_pos = 0;

		while (scan_pos < impl_->write_pos) {
			// Find the next STX marker.
			std::size_t stx_pos = impl_->find_stx(scan_pos);

			if (stx_pos == impl_->write_pos) {
				// No STX found. Discard everything before write_pos, but keep
				// the last 3 bytes (partial STX could span across feeds).
				if (impl_->write_pos > 3) {
					scan_pos = impl_->write_pos - 3;
				}
				break;
			}

			// Discard any bytes before the STX (garbage / partial frames).
			if (stx_pos > scan_pos) {
				scan_pos = stx_pos;
			}

			// Try to extract a complete frame at stx_pos.
			std::size_t consumed = impl_->try_extract_frame(stx_pos);

			if (consumed == 0) {
				// Incomplete frame — wait for more data.
				break;
			}

			scan_pos = stx_pos + consumed;
		}

		// Compact: remove fully processed bytes.
		impl_->compact(scan_pos);
	}


	void FrameReceiver::Reset() {
		impl_->write_pos = 0;
	}


	std::uint8_t FrameReceiver::ComputeCrc8(const std::uint8_t *data, std::size_t len) {
		return Impl::compute_crc8_internal(data, len);
	}


}  // namespace LMS4xxx
