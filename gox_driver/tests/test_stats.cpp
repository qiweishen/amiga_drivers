// Tests for CameraStats/StatsReporter (core/stats.cpp) and the ChunkPool
// (core/chunk_pool.cpp).

#include "core/stats.hpp"

#include "core/chunk_pool.hpp"
#include "core/frame.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("stats: snapshot copies every counter") {
    jai::CameraStats st;
    st.frames_retrieved_ok.store(11);
    st.frames_incomplete.store(2);
    st.frames_error_dropped.store(3);
    st.frames_dropped_queue.store(4);
    st.blockid_gap_events.store(5);
    st.frames_lost_gap.store(6);
    st.retrieve_timeouts.store(7);
    st.frames_written.store(8);
    st.bytes_written.store(9);
    st.segments_created.store(10);
    st.stream_blocks_dropped.store(12);
    st.stream_error_count.store(13);
    st.queue_depth.store(14);
    st.queue_capacity.store(15);

    const jai::CameraStats::Snapshot s = st.snapshot();
    CHECK(s.frames_retrieved_ok == 11u);
    CHECK(s.frames_incomplete == 2u);
    CHECK(s.frames_error_dropped == 3u);
    CHECK(s.frames_dropped_queue == 4u);
    CHECK(s.blockid_gap_events == 5u);
    CHECK(s.frames_lost_gap == 6u);
    CHECK(s.retrieve_timeouts == 7u);
    CHECK(s.frames_written == 8u);
    CHECK(s.bytes_written == 9u);
    CHECK(s.segments_created == 10u);
    CHECK(s.stream_blocks_dropped == 12u);
    CHECK(s.stream_error_count == 13u);
    CHECK(s.queue_depth == 14u);
    CHECK(s.queue_capacity == 15u);
}

TEST_CASE("stats: periodic_line computes rates against the previous snapshot") {
    jai::CameraStats st;
    st.frames_retrieved_ok.store(101);
    st.frames_incomplete.store(1);
    st.frames_written.store(100);
    st.bytes_written.store(1000000);
    st.segments_created.store(2);
    st.queue_depth.store(3);
    st.queue_capacity.store(32);

    jai::StatsReporter rep("cam0", &st);
    const std::string line = rep.periodic_line(/*interval_s=*/2.0, /*uptime_s=*/65,
                                               /*free_disk_bytes=*/1ull << 30);
    CAPTURE(line);
    CHECK(contains(line, "[cam0]"));
    CHECK(contains(line, "up=00:01:05"));
    CHECK(contains(line, "fps=50.0"));     // 100 frames / 2 s against the zero snapshot
    CHECK(contains(line, "disk=0.5MB/s")); // 1e6 bytes / 2 s
    CHECK(contains(line, "ok=101"));
    CHECK(contains(line, "incomp=1"));
    CHECK(contains(line, "drop_q=0"));
    CHECK(contains(line, "q=3/32"));
    CHECK(contains(line, "seg=2"));
    CHECK(contains(line, "free=1.00 GiB"));

    // Second call: rates come from the delta, not the absolute counters.
    st.frames_written.store(150);
    st.bytes_written.store(3000000);
    const std::string line2 = rep.periodic_line(2.0, 67, 1ull << 30);
    CAPTURE(line2);
    CHECK(contains(line2, "up=00:01:07"));
    CHECK(contains(line2, "fps=25.0"));     // (150 - 100) / 2
    CHECK(contains(line2, "disk=1.0MB/s")); // (3e6 - 1e6) / 2

    // A zero interval must not divide by zero.
    const std::string line3 = rep.periodic_line(0.0, 67, 0);
    CHECK(contains(line3, "fps=0.0"));
    CHECK(contains(line3, "disk=0.0MB/s"));
}

TEST_CASE("stats: final_summary contains the session totals") {
    jai::CameraStats st;
    st.frames_retrieved_ok.store(1000);
    st.frames_incomplete.store(2);
    st.frames_dropped_queue.store(3);
    st.frames_error_dropped.store(4);
    st.blockid_gap_events.store(5);
    st.frames_lost_gap.store(6);
    st.stream_blocks_dropped.store(7);
    st.stream_error_count.store(8);
    st.frames_written.store(997);
    st.bytes_written.store(3000000);
    st.segments_created.store(2);

    jai::StatsReporter rep("cam1", &st);
    const std::string s = rep.final_summary(/*uptime_s=*/3661);
    CAPTURE(s);
    CHECK(contains(s, "[cam1]"));
    CHECK(contains(s, "session summary"));
    CHECK(contains(s, "duration=01:01:01"));
    CHECK(contains(s, "frames_ok=1000"));
    CHECK(contains(s, "incomplete=2"));
    CHECK(contains(s, "dropped_queue=3"));
    CHECK(contains(s, "dropped_error=4"));
    CHECK(contains(s, "blockid_gaps=5"));
    CHECK(contains(s, "frames_lost=6"));
    CHECK(contains(s, "stream_blocks_dropped=7"));
    CHECK(contains(s, "stream_errors=8"));
    CHECK(contains(s, "frames_written=997"));
    CHECK(contains(s, "bytes=2.86 MiB")); // 3000000 / 2^20
    CHECK(contains(s, "segments=2"));
}

TEST_CASE("chunk_pool: acquire until exhaustion, release restores capacity") {
    jai::ChunkPool pool(3, 256);
    CHECK(pool.capacity() == 3u);
    CHECK(pool.chunk_bytes() == 256u);
    CHECK(pool.available() == 3u);

    std::vector<jai::FrameChunkPtr> held;
    for (int i = 0; i < 3; ++i) {
        jai::FrameChunkPtr c = pool.acquire();
        REQUIRE(c != nullptr);
        CHECK(c->capacity == 256u);
        CHECK(c->data != nullptr);
        held.push_back(std::move(c));
    }
    CHECK(pool.available() == 0u);
    CHECK(pool.acquire() == nullptr); // exhausted: returns nullptr, never blocks
    CHECK(pool.acquire() == nullptr); // still exhausted

    // Release one chunk with dirty metadata; a fresh acquire gets it back
    // with the metadata reset.
    held[0]->meta.block_id = 42;
    held[0]->meta.status_flags = 0xFF;
    pool.release(std::move(held[0]));
    CHECK(pool.available() == 1u);
    jai::FrameChunkPtr again = pool.acquire();
    REQUIRE(again != nullptr);
    CHECK(again->meta.block_id == 0u);
    CHECK(again->meta.status_flags == 0u);

    pool.release(nullptr); // no-op
    CHECK(pool.available() == 0u);

    pool.release(std::move(again));
    pool.release(std::move(held[1]));
    pool.release(std::move(held[2]));
    CHECK(pool.available() == pool.capacity()); // everything returned
    jai::FrameChunkPtr c = pool.acquire();
    CHECK(c != nullptr);
    pool.release(std::move(c));
}
