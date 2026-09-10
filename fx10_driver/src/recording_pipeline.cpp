#include "recording_pipeline.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include "logger.h"

namespace fx10 {
    namespace { common::DriverLog g_log{"FX10"}; }

    RecordingPipeline::RecordingPipeline(IFrameSink &sink, Options options)
        : sink_(sink), options_(std::move(options)), pixel_info_(GetPixelFormatInfo(options_.pixel_format)) {
        if (!pixel_info_ || options_.width == 0 || options_.height == 0 ||
            options_.payload_capacity == 0 || options_.queue_frames == 0 ||
            options_.queue_frames > std::numeric_limits<std::uint32_t>::max() - 2u ||
            options_.height > std::numeric_limits<std::size_t>::max() / options_.width / 2 ||
            (pixel_info_->packed && options_.width % 2 != 0)) {
            throw std::invalid_argument("invalid recording pipeline geometry or capacity");
        }
        const auto pixels = static_cast<std::size_t>(options_.width) * options_.height;
        if (WireBytes(*pixel_info_, options_.width) * options_.height > options_.payload_capacity)
            throw std::invalid_argument("pipeline payload capacity is smaller than the pixel image");
        const auto canonical_bytes = pixels * pixel_info_->storage_bpp;
        canonical_.resize((canonical_bytes + 1) / 2);
        slots_.resize(static_cast<std::size_t>(options_.queue_frames) + 2);
        ready_.resize(options_.queue_frames);
        free_.reserve(slots_.size());
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            slots_[i].bytes.resize(options_.payload_capacity);
            free_.push_back(i);
        }
        worker_ = std::thread(&RecordingPipeline::Run, this);
    }

    RecordingPipeline::~RecordingPipeline() { Finish(); }

    void RecordingPipeline::Fail(const std::string &error) {
        std::lock_guard lock(error_mutex_);
        if (!failed_.load(std::memory_order_relaxed)) {
            error_ = error;
            failed_.store(true, std::memory_order_release);
            g_log.Error("[Writer] {}", error);
        }
    }

    std::string RecordingPipeline::ErrorMessage() const {
        std::lock_guard lock(error_mutex_);
        return error_.empty() && sink_.Failed() ? "frame sink failed (see recorder error)" : error_;
    }

    bool RecordingPipeline::Submit(const FrameView &frame, std::uint64_t first_missing,
                                   std::uint64_t missing_count, Rejection rejection) {
        if (Failed()) return false;
        if (rejection == Rejection::kNone && (!frame.data || frame.size == 0 ||
            frame.size > options_.payload_capacity)) {
            Fail("invalid input size for recording queue at BlockID " + std::to_string(frame.block_id));
            return false;
        }
        std::size_t index;
        {
            std::lock_guard lock(mutex_);
            if (closed_ || ready_count_ == ready_.size() || free_.empty()) {
                Fail("recording queue closed or full at BlockID " + std::to_string(frame.block_id));
                return false;
            }
            index = free_.back();
            free_.pop_back();
        }
        auto &slot = slots_[index];
        slot.frame = frame;
        slot.first_missing = first_missing;
        slot.missing_count = missing_count;
        slot.rejection = rejection;
        if (rejection == Rejection::kNone) {
            std::memcpy(slot.bytes.data(), frame.data, frame.size);
            slot.frame.data = slot.bytes.data();
        } else {
            slot.frame.data = nullptr;
            slot.frame.size = 0;
        }
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                free_.push_back(index);
                Fail("recording queue closed during submit");
                return false;
            }
            // Single producer: ready_count cannot increase while copying.
            ready_[tail_] = index;
            tail_ = (tail_ + 1) % ready_.size();
            ++ready_count_;
        }
        ready_cv_.notify_one();
        return true;
    }

    void RecordingPipeline::Finish() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    void RecordingPipeline::Run() {
        bool sink_failed = false;
        for (;;) {
            std::size_t index;
            {
                std::unique_lock lock(mutex_);
                ready_cv_.wait(lock, [this] { return closed_ || ready_count_ != 0; });
                if (ready_count_ == 0) return;
                index = ready_[head_];
                head_ = (head_ + 1) % ready_.size();
                --ready_count_;
            }
            auto &slot = slots_[index];
            bool attempted = false;
            try {
                if (!sink_failed && !sink_.Failed()) {
                    attempted = true;
                    Deliver(slot);
                } else {
                    sink_failed = true;
                }
                if (sink_.Failed()) sink_failed = true;
                if (sink_failed) Fail("frame sink failed while draining the recording queue");
            } catch (const std::exception &error) {
                sink_failed = true;
                Fail(std::string("recording worker failed: ") + error.what());
            } catch (...) {
                sink_failed = true;
                Fail("recording worker failed with an unknown exception");
            }
            if ((!attempted || sink_failed) && slot.rejection == Rejection::kNone)
                unconfirmed_.fetch_add(1, std::memory_order_relaxed);
            // A producer-side overflow still drains every accepted item. A sink
            // failure releases the rest without pretending they were written.
            {
                std::lock_guard lock(mutex_);
                free_.push_back(index);
            }
        }
    }

    void RecordingPipeline::Deliver(Slot &slot) {
        if (slot.rejection != Rejection::kNone) {
            const char *reason = slot.rejection == Rejection::kOperationError ? "operation-error" :
                                 slot.rejection == Rejection::kNonImage ? "non-image" : "layout-error";
            sink_.OnRejected(slot.frame.block_id, reason, &slot.frame);
            return;
        }
        auto frame = slot.frame;
        if (!frame.sdk || !frame.sdk->image_present || frame.width != options_.width ||
            frame.height != options_.height || frame.bytes_per_pixel != pixel_info_->storage_bpp)
            throw std::runtime_error("recording queue image geometry mismatch");
        const auto row_bytes = WireBytes(*pixel_info_, options_.width);
        const std::uint64_t stride = row_bytes + static_cast<std::uint64_t>(frame.sdk->padding_x);
        // Use division before multiplication for metadata originating outside the application.
        if (stride > frame.size / options_.height ||
            stride * options_.height + frame.sdk->padding_y != frame.size)
            throw std::runtime_error("recording queue image padding mismatch");
        auto *canonical = reinterpret_cast<std::uint8_t *>(canonical_.data());
        for (std::size_t row = 0; row < options_.height; ++row) {
            const auto *src = slot.bytes.data() + row * static_cast<std::size_t>(stride);
            if (pixel_info_->packed) {
                auto *dst = canonical_.data() + row * options_.width;
                if (options_.pixel_format == "Mono12Packed") UnpackMono12Packed(src, options_.width, dst);
                else UnpackMono10Packed(src, options_.width, dst);
            } else {
                std::memcpy(canonical + row * row_bytes, src, row_bytes);
            }
        }
        frame.data = canonical;
        frame.size = static_cast<std::size_t>(options_.width) * options_.height * pixel_info_->storage_bpp;
        if (!preamble_checked_) {
            preamble_checked_ = true;
            CheckStatusLine(frame);
        }
        if (slot.missing_count != 0) sink_.OnGap(slot.first_missing, slot.missing_count);
        if (!sink_.Failed()) {
            sink_.OnFrame(frame);
            delivered_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void RecordingPipeline::CheckStatusLine(const FrameView &frame) {
        if (options_.status_line || frame.bytes_per_pixel != 2 || frame.width < 4) return;
        const auto offset = static_cast<std::size_t>(frame.height - 1) * frame.width * 2;
        if (frame.data[offset] == 0xFF && frame.data[offset + 2] == 0x00 &&
            frame.data[offset + 4] == 0xBB && frame.data[offset + 6] == 0x66) {
            g_log.Warn("[eBUS] Status-line preamble detected but status_line=off; "
                       "the last band row may contain metadata, check EnStatusLine");
        }
    }
}
