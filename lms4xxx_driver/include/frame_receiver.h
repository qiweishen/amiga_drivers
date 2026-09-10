#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>


namespace lms4xxx {
    // Data section of one CRC-validated CoLa B frame (STX/Length/CS stripped)
    struct RawFrame {
        std::vector<std::uint8_t> data; ///< Data payload (command + params)
        std::uint64_t receive_timestamp_us = 0; ///< Host timestamp (microseconds)

        RawFrame() = default;

        explicit RawFrame(std::size_t capacity) { data.reserve(capacity); }

        RawFrame(const RawFrame &) = default;

        RawFrame &operator=(const RawFrame &) = default;

        RawFrame(RawFrame &&) noexcept = default;

        RawFrame &operator=(RawFrame &&) noexcept = default;
    };


    // Splits the TCP byte stream into CRC-validated CoLa B frames
    class FrameReceiver {
    public:
        using FrameCallback = std::function<void(RawFrame &&frame)>;

        // Why a frame was rejected. Typed rather than a string: the driver used
        // to classify these by substring-matching the reason text, which a
        // reworded message would have silently reassigned to the wrong counter.
        enum class FrameError {
            kLengthOutOfRange, ///< the length field is zero or exceeds max_frame_size
            kChecksumMismatch, ///< XOR over the data part disagrees ("CRC" in the GUI stats)
            kGarbage, ///< bytes between frames that belong to no telegram
        };

        // `consecutive` counts rejected candidates and garbage spans since the
        // last GOOD frame, excluding discarded tails of reported candidates.
        // It is a resync fault metric, not a count of lost device scans.
        using ErrorCallback = std::function<void(FrameError error, std::uint64_t consecutive)>;

        // `tag`: instance log prefix
        FrameReceiver(FrameCallback on_frame, ErrorCallback on_error, std::size_t max_frame_size = 64 * 1024,
                      std::string tag = {});

        ~FrameReceiver();

        FrameReceiver(const FrameReceiver &) = delete;

        FrameReceiver &operator=(const FrameReceiver &) = delete;

        FrameReceiver(FrameReceiver &&) noexcept;

        FrameReceiver &operator=(FrameReceiver &&) noexcept;

        // Complete frames are delivered through on_frame
        void Feed(const std::uint8_t *data, std::size_t len);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lms4xxx
