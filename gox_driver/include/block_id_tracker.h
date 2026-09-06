#pragma once

// GVSP BlockID gap accounting, split out of the acquisition loop so it is
// exercised without a camera. With GevGVSPExtendedIDMode=On (forced by
// build_apply_plan) the IDs are 64-bit and never wrap, so a gap is always a
// genuine network loss.

#include <cstdint>

namespace gox {
    struct BlockIdGap {
        bool gap = false; // a gap was detected on this frame
        uint64_t missing = 0; // number of BlockIDs that never arrived
    };

    class BlockIdTracker {
    public:
        // Records one arrived BlockID and reports the gap in front of it. Must be
        // called for EVERY retrieved buffer, including ones that are dropped
        // afterwards - skipping one fabricates a gap on the next frame.
        BlockIdGap Observe(uint64_t block_id);

        bool HasLast() const { return have_last_; }
        uint64_t Last() const { return last_; }

    private:
        bool have_last_ = false;
        uint64_t last_ = 0;
    };
} // namespace gox
