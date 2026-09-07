#include "frame_receiver.h"

#include "byte_util.h"
#include "cola_b.h"
#include "time_util.h"

#include <chrono>
#include <algorithm>
#include <cstring>
#include <string>

#include "logger.h"
#include "utility.h"


namespace {
    constexpr std::string_view kModule = "LMS4xxx";

    common::DriverLog g_log{std::string(kModule)};

    constexpr std::size_t kStxSize = 4;

    // STX(4) + Length(4)
    constexpr std::size_t kHeaderSize = kStxSize + 4;
} // namespace


namespace lms4xxx {
    struct FrameReceiver::Impl {
        FrameCallback on_frame;
        ErrorCallback on_error;
        std::size_t max_frame_size;
        std::string tag; // instance log-line prefix

        // Linear accumulation buffer, sized for two max frames
        std::vector<std::uint8_t> buffer;
        std::size_t write_pos = 0;
        // End offset of bytes already accounted for by a rejected candidate.
        // Resync still searches inside that region for a recoverable frame,
        // but discarding its remainder is not another garbage error. This
        // survives Compact()/Feed() so splitting a bad frame cannot double-count it.
        std::size_t error_covered_until = 0;

        // Log the first error per burst and then a periodic summary. The
        // callback counts rejected candidates and unaccounted garbage spans,
        // not a known number of lost device scans.
        std::uint64_t consecutive_framing_errors = 0;
        std::chrono::steady_clock::time_point last_framing_log{};

        void ReportFramingError(FrameReceiver::FrameError error, const char *what, const std::string &detail) {
            ++consecutive_framing_errors;
            if (on_error) {
                on_error(error, consecutive_framing_errors);
            }
            const auto now = std::chrono::steady_clock::now();
            const bool first = consecutive_framing_errors == 1;
            if (first || now - last_framing_log >= std::chrono::seconds(1)) {
                last_framing_log = now;
                g_log.Warn("[{}] {} ({}); resynchronising — {} consecutive framing error(s)", tag, detail, what,
                           consecutive_framing_errors);
            }
        }

        void ReportGarbage(std::size_t begin, std::size_t end, const char *what) {
            const auto unaccounted_begin = std::max(begin, error_covered_until);
            if (end > unaccounted_begin) {
                ReportFramingError(FrameReceiver::FrameError::kGarbage, what,
                                   fmt::format("{} byte(s) discarded", end - unaccounted_begin));
            }
        }

        Impl(FrameCallback on_frame_cb, ErrorCallback on_error_cb, std::size_t max_frame,
             std::string tag_str) : on_frame(std::move(on_frame_cb)), on_error(std::move(on_error_cb)),
                                    max_frame_size(max_frame),
                                    tag(std::move(tag_str)) {
            buffer.resize(max_frame_size * 2 + kHeaderSize + 1);
        }

        // First STX at or after `offset`; write_pos when none
        std::size_t FindStx(std::size_t offset) const {
            if (write_pos < offset + kStxSize) {
                return write_pos;
            }
            const std::size_t search_end = write_pos - kStxSize + 1;
            for (std::size_t i = offset; i < search_end; ++i) {
                if (buffer[i] == 0x02 && buffer[i + 1] == 0x02 && buffer[i + 2] == 0x02 && buffer[i + 3] == 0x02) {
                    return i;
                }
            }
            return write_pos;
        }

        // Drop the consumed bytes [0, offset)
        void Compact(std::size_t offset) {
            if (offset == 0) {
                return;
            }
            error_covered_until = error_covered_until > offset ? error_covered_until - offset : 0;
            if (offset >= write_pos) {
                write_pos = 0;
                return;
            }
            std::size_t remaining = write_pos - offset;
            std::memmove(buffer.data(), buffer.data() + offset, remaining);
            write_pos = remaining;
        }

        // Bytes consumed by the frame at `pos`; 0 = incomplete
        std::size_t TryExtractFrame(std::size_t pos) {
            std::size_t available = write_pos - pos;

            if (available < kHeaderSize) {
                return 0;
            }

            std::uint32_t data_len = common::ByteUtil::LoadBigU32(buffer.data() + pos + kStxSize);

            if (data_len == 0 || data_len > max_frame_size) {
                // An out-of-range length cannot justify skipping its claimed
                // payload. Only its header is known; for length zero the sole
                // remaining byte is the empty telegram's checksum.
                const auto rejected_end = pos + kHeaderSize + (data_len == 0 ? 1u : 0u);
                error_covered_until = std::max(error_covered_until, rejected_end);
                ReportFramingError(FrameReceiver::FrameError::kLengthOutOfRange,
                                     "frame data length out of range",
                                     fmt::format("length {} not in 1..{}", data_len, max_frame_size));
                // STX can overlap this candidate (e.g. one stray 0x02 before
                // a valid frame). Skipping all four bytes would lose it.
                return 1;
            }

            std::size_t total_frame_size = kHeaderSize + data_len + 1;

            if (available < total_frame_size) {
                return 0; // Need more data.
            }

            const std::uint8_t *data_start = buffer.data() + pos + kHeaderSize;
            std::uint8_t received_cs = buffer[pos + kHeaderSize + data_len];

            std::uint8_t computed_cs = ColaBCodec::ComputeChecksum(data_start, data_len);

            if (computed_cs != received_cs) {
                error_covered_until = std::max(error_covered_until, pos + total_frame_size);
                // The manual calls this "CRC8" (p.66) but specifies XOR; the
                // name is kept because the GUI statistics field is crc=.
                ReportFramingError(FrameReceiver::FrameError::kChecksumMismatch, "checksum mismatch",
                                     fmt::format("computed 0x{:02X}, received 0x{:02X}", computed_cs, received_cs));
                return 1; // search every possible STX without charging its tail again
            }

            consecutive_framing_errors = 0; // a good frame ends the burst
            error_covered_until = 0;

            RawFrame frame;
            frame.data.assign(data_start, data_start + data_len);
            frame.receive_timestamp_us = common::TimeUtil::SteadyNowUs();

            if (on_frame) {
                on_frame(std::move(frame));
            }

            return total_frame_size;
        }
    };


    FrameReceiver::FrameReceiver(FrameCallback on_frame, ErrorCallback on_error, std::size_t max_frame_size,
                                 std::string tag) : impl_(std::make_unique<Impl>(
        std::move(on_frame), std::move(on_error), max_frame_size, std::move(tag))) {
    }


    FrameReceiver::~FrameReceiver() = default;


    FrameReceiver::FrameReceiver(FrameReceiver &&) noexcept = default;

    FrameReceiver &FrameReceiver::operator=(FrameReceiver &&) noexcept = default;


    void FrameReceiver::Feed(const std::uint8_t *data, std::size_t len) {
        if (len == 0) {
            return;
        }

        // Should not happen with the pre-allocation
        if (impl_->write_pos + len > impl_->buffer.size()) {
            impl_->buffer.resize(impl_->write_pos + len + impl_->max_frame_size);
        }

        std::memcpy(impl_->buffer.data() + impl_->write_pos, data, len);
        impl_->write_pos += len;

        std::size_t scan_pos = 0;

        while (scan_pos < impl_->write_pos) {
            std::size_t stx_pos = impl_->FindStx(scan_pos);

            if (stx_pos == impl_->write_pos) {
                // No STX: keep the last 3 bytes (a partial STX may span feeds) but never rewind
                // into a frame that was already delivered
                if (impl_->write_pos > 3) {
                    const auto keep_from = std::max(scan_pos, impl_->write_pos - 3);
                    impl_->ReportGarbage(scan_pos, keep_from, "no STX in received bytes");
                    scan_pos = keep_from;
                }
                break;
            }

            if (stx_pos > scan_pos) {
                // Bytes between frames are not CoLa B: count them so a garbage stream trips the fault
                impl_->ReportGarbage(scan_pos, stx_pos, "bytes skipped before STX");
                scan_pos = stx_pos;
            }

            std::size_t consumed = impl_->TryExtractFrame(stx_pos);

            if (consumed == 0) {
                break;
            }

            scan_pos = stx_pos + consumed;
        }

        impl_->Compact(scan_pos);
    }


} // namespace lms4xxx
