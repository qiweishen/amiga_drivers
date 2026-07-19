#pragma once

// Fixed-size pool of FrameChunk buffers. The acquisition thread acquires a
// chunk, memcpy's the PvBuffer payload into it and pushes it down the queue;
// the writer thread releases it after the bytes hit the file. Pool
// exhaustion means the writer is falling behind — the caller counts that as
// a dropped frame (drop_newest policy).

#include <cstddef>
#include <mutex>
#include <vector>

#include "core/frame.hpp"

namespace jai {

	class ChunkPool {
	public:
		// Pre-allocates `count` chunks of `chunk_bytes` each.
		ChunkPool(size_t count, size_t chunk_bytes);

		// nullptr when the pool is exhausted (never blocks).
		FrameChunkPtr acquire();

		// Returns a chunk to the pool. Chunks from other pools are rejected in
		// debug via capacity check; in release they are simply adopted.
		void release(FrameChunkPtr chunk);

		size_t capacity() const { return capacity_; }
		size_t chunk_bytes() const { return chunk_bytes_; }
		size_t available() const;

	private:
		const size_t capacity_;
		const size_t chunk_bytes_;
		mutable std::mutex mutex_;
		std::vector<FrameChunkPtr> free_list_;
	};

}  // namespace jai
