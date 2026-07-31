#include "accounting.hpp"


namespace fx10 {
    namespace {
        constexpr std::uint64_t kMax16 = 0xFFFF; // highest valid 16-bit BlockID
        constexpr std::uint64_t kCycle16 = 0xFFFF; // 65535 valid IDs (1..65535, 0 skipped)
        constexpr std::uint64_t kHalfCycle16 = 0x7FFF; // forward-vs-backwards decision threshold
    } // namespace


    BlockIdTracker::Observation BlockIdTracker::observe(std::uint64_t block_id) {
        Observation out;
        ++observed_;

        if (block_id > kMax16) {
            saw_wide_ = true;
        }

        if (first_) {
            first_ = false;
            prev_ = block_id;
            return out;
        }

        const bool sixteen_bit = mode_ == Mode::k16Bit || (mode_ == Mode::kAuto && !saw_wide_);

        if (sixteen_bit) {
            if (block_id == 0 || block_id > kMax16) {
                // ID 0 never appears on a 16-bit GVSP wire (skipped on wrap).
                ++anomalies_;
                out.anomaly = true;
                prev_ = block_id == 0 ? prev_ : block_id; // keep prev on invalid 0
                return out;
            }
            const std::uint64_t expected = prev_ >= kMax16 ? 1 : prev_ + 1;
            if (block_id == expected) {
                prev_ = block_id;
                return out;
            }
            // Forward distance in the 1..65535 cycle (0 skipped).
            const std::uint64_t dist = (block_id + kCycle16 - expected) % kCycle16;
            if (dist >= 1 && dist <= kHalfCycle16) {
                out.gap_before = dist;
                out.first_missing = expected;
                total_missed_ += dist;
            } else {
                // Backwards / duplicate — reordering, not a loss.
                ++anomalies_;
                out.anomaly = true;
            }
            prev_ = block_id;
            return out;
        }

        // 64-bit mode: strictly increasing.
        if (block_id <= prev_) {
            ++anomalies_;
            out.anomaly = true;
            prev_ = block_id;
            return out;
        }
        out.gap_before = block_id - prev_ - 1;
        if (out.gap_before > 0) out.first_missing = prev_ + 1;
        total_missed_ += out.gap_before;
        prev_ = block_id;
        return out;
    }


    RunStatus classify(const Counters &c) {
        const bool clean = c.frames_missed_rx == 0 && c.op_errors == 0 &&
                           c.size_mismatch_drops == 0 && c.write_errors == 0 &&
                           c.blockid_anomalies == 0 && c.missed_trigger_delta <= 0;
        return clean ? RunStatus::kClean : RunStatus::kDegraded;
    }


    const char *toString(RunStatus status) {
        return status == RunStatus::kClean ? "CLEAN" : "DEGRADED";
    }
} // namespace fx10
