#include "chunk_pool.h"

namespace gox {
    ChunkPool::ChunkPool(size_t count, size_t chunk_bytes) : capacity_(count), chunk_bytes_(chunk_bytes) {
        free_list_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            free_list_.emplace_back(new FrameChunk(chunk_bytes));
        }
    }

    FrameChunkPtr ChunkPool::Acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_list_.empty()) {
            return nullptr;
        }
        FrameChunkPtr chunk = std::move(free_list_.back());
        free_list_.pop_back();
        chunk->meta = FrameMeta{};
        return chunk;
    }

    void ChunkPool::Release(FrameChunkPtr chunk) {
        if (!chunk) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_list_.size() < capacity_) {
            free_list_.push_back(std::move(chunk));
        }
        // else: excess chunk is destroyed (should not happen in practice)
    }

    size_t ChunkPool::Available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_list_.size();
    }
} // namespace gox
