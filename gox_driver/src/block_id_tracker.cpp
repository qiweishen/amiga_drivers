#include "block_id_tracker.h"

namespace gox {
    BlockIdGap BlockIdTracker::Observe(uint64_t block_id) {
        BlockIdGap out;
        if (have_last_ && block_id > last_ + 1) {
            out.gap = true;
            out.missing = block_id - last_ - 1;
        }
        have_last_ = true;
        last_ = block_id;
        return out;
    }
} // namespace gox
