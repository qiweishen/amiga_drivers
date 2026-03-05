#ifndef LMS4XXX_FRAME_RECEIVER_H
#define LMS4XXX_FRAME_RECEIVER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>


namespace LMS4xxx {

	// A raw CoLa B frame extracted from the TCP byte stream.
	//
	// Layout: [STX 4B] [Length 4B] [Data ...] [Checksum 1B]
	// This struct holds only the data portion (without STX/Length/CS).
	// The CRC has already been validated by FrameReceiver.
	struct RawFrame {
		std::vector<std::uint8_t> data;			 ///< Data payload (command + params)
		std::uint64_t receive_timestamp_us = 0;	 ///< Host timestamp (microseconds)

		RawFrame() = default;
		explicit RawFrame(std::size_t capacity) { data.reserve(capacity); }

		RawFrame(const RawFrame &) = default;
		RawFrame &operator=(const RawFrame &) = default;
		RawFrame(RawFrame &&) noexcept = default;
		RawFrame &operator=(RawFrame &&) noexcept = default;
	};


	// CoLa B frame boundary detector and extractor.
	class FrameReceiver {
	public:
		// Callback for a complete, CRC-validated frame.
		using FrameCallback = std::function<void(RawFrame &&frame)>;

		// Callback for framing/CRC errors (for statistics).
		using ErrorCallback = std::function<void(const char *reason)>;

		FrameReceiver(FrameCallback on_frame, ErrorCallback on_error, std::size_t max_frame_size = 64 * 1024);

		~FrameReceiver();

		FrameReceiver(const FrameReceiver &) = delete;
		FrameReceiver &operator=(const FrameReceiver &) = delete;
		FrameReceiver(FrameReceiver &&) noexcept;
		FrameReceiver &operator=(FrameReceiver &&) noexcept;

		// Feed raw TCP bytes into the frame detector.
		// Complete frames are delivered via the on_frame callback.
		void Feed(const std::uint8_t *data, std::size_t len);

		// Reset internal state (e.g., after reconnection).
		void Reset();

		// Compute CRC8 checksum (XOR over all bytes).
		[[nodiscard]] static std::uint8_t ComputeCrc8(const std::uint8_t *data, std::size_t len);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

}  // namespace LMS4xxx

#endif	// LMS4XXX_FRAME_RECEIVER_H
