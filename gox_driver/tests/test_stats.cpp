// Tests for CameraStats/StatsReporter (core/stats.cpp) and the ChunkPool
// (core/chunk_pool.cpp).

#include "../include/stats.h"

#include "../include/chunk_pool.h"
#include "../include/frame.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {
    bool Contains(const std::string &s, const std::string &needle) {
        return s.find(needle) != std::string::npos;
    }
} // namespace

TEST_CASE("stats: snapshot copies every counter") {
    gox::CameraStats st;
    st.frames_retrieved_ok.store(11);
    st.frames_incomplete.store(2);
    st.frames_error_dropped.store(3);
    st.frames_dropped_queue.store(4);
    st.blockid_gap_events.store(5);
    st.frames_lost_gap.store(6);
    st.frames_written.store(8);
    st.bytes_written.store(9);
    st.segments_created.store(10);
    st.stream_blocks_dropped.store(12);
    st.stream_error_count.store(13);
    st.queue_depth.store(14);
    st.queue_capacity.store(15);
    st.sensor_temp_centi.store(4230);
    st.trigger_count.store(16);
    st.trigger_overflow.store(true);

    const gox::CameraStats::Snapshot s = st.GetSnapshot();
    CHECK(s.frames_retrieved_ok == 11u);
    CHECK(s.frames_incomplete == 2u);
    CHECK(s.frames_error_dropped == 3u);
    CHECK(s.frames_dropped_queue == 4u);
    CHECK(s.blockid_gap_events == 5u);
    CHECK(s.frames_lost_gap == 6u);
    CHECK(s.frames_written == 8u);
    CHECK(s.bytes_written == 9u);
    CHECK(s.segments_created == 10u);
    CHECK(s.stream_blocks_dropped == 12u);
    CHECK(s.stream_error_count == 13u);
    CHECK(s.queue_depth == 14u);
    CHECK(s.queue_capacity == 15u);
    CHECK(s.sensor_temp_centi == 4230);
    CHECK(s.trigger_count == 16);
    CHECK(s.trigger_overflow);
}

TEST_CASE("stats: periodic_line computes rates against the previous snapshot") {
    gox::CameraStats st;
    st.frames_retrieved_ok.store(101);
    st.frames_incomplete.store(1);
    st.frames_written.store(100);
    st.bytes_written.store(1000000);
    st.segments_created.store(2);
    st.queue_depth.store(3);
    st.queue_capacity.store(32);

    gox::StatsReporter rep("cam0", &st);
    const std::string line = rep.PeriodicLine(/*interval_s=*/2.0, /*uptime_s=*/65);
    CAPTURE(line);
    CHECK(Contains(line, "[cam0]"));
    CHECK(Contains(line, "up=00:01:05"));
    CHECK(Contains(line, "[Statistics] [cam0]"));
    CHECK(Contains(line, "rate=51.0 Hz")); // (101 ok + 1 incomplete) / 2 s: sensor output rate
    CHECK(Contains(line, "fps=50.0")); // 100 frames / 2 s against the zero snapshot
    CHECK(Contains(line, "disk=0.5 MB/s")); // 1e6 bytes / 2 s
    CHECK(Contains(line, "ok=101"));
    CHECK(Contains(line, "incomp=1"));
    CHECK(Contains(line, "drop_q=0"));
    CHECK(Contains(line, "drop_net=0"));
    CHECK(Contains(line, "gaps=0(-0)"));
    CHECK(Contains(line, "q=3/32"));
    CHECK(Contains(line, "seg=2"));
    CHECK(Contains(line, "written=976.56 KiB"));
    CHECK_FALSE(Contains(line, "free=")); // free space is rig-wide now; Main reports it

    // Second call: rates come from the delta, not the absolute counters.
    st.frames_written.store(150);
    st.bytes_written.store(3000000);
    const std::string line2 = rep.PeriodicLine(2.0, 67);
    CAPTURE(line2);
    CHECK(Contains(line2, "up=00:01:07"));
    CHECK(Contains(line2, "rate=0.0 Hz")); // no new frames from the camera this interval
    CHECK(Contains(line2, "fps=25.0")); // (150 - 100) / 2
    CHECK(Contains(line2, "disk=1.0 MB/s")); // (3e6 - 1e6) / 2

    // A zero interval must not divide by zero.
    const std::string line3 = rep.PeriodicLine(0.0, 67);
    CHECK(Contains(line3, "rate=0.0 Hz"));
    CHECK(Contains(line3, "fps=0.0"));
    CHECK(Contains(line3, "disk=0.0 MB/s"));
}

TEST_CASE("stats: the device telemetry keys only appear once there is a reading") {
    gox::CameraStats st;
    st.frames_written.store(100);
    gox::StatsReporter rep("cam0", &st);

    SUBCASE("nothing polled yet: the line ends exactly where it always did") {
        const std::string line = rep.PeriodicLine(2.0, 65);
        CAPTURE(line);
        CHECK_FALSE(Contains(line, "temp="));
        CHECK_FALSE(Contains(line, "trig="));
        const std::string tail = "written=0 B"; // bytes_written is untouched in this case
        REQUIRE(line.size() >= tail.size());
        CHECK(line.substr(line.size() - tail.size()) == tail);
    }

    SUBCASE("temperature and trigger count are appended, after fps=") {
        st.sensor_temp_centi.store(4230);
        st.trigger_count.store(17);
        const std::string line = rep.PeriodicLine(2.0, 65);
        CAPTURE(line);
        CHECK(Contains(line, "written=0 B  temp=42.3  trig=17"));
        // The GUI's fps regex must keep matching, and the new keys must never
        // come before it.
        CHECK(line.find("fps=50.0") < line.find("temp="));
        CHECK(line.find("temp=") < line.find("trig="));
    }

    SUBCASE("a wrapped 32-bit counter is flagged in place") {
        st.trigger_count.store(17);
        st.trigger_overflow.store(true);
        CHECK(Contains(rep.PeriodicLine(2.0, 65), "trig=17(ovf)"));
    }

    SUBCASE("freerun has a temperature but no counter") {
        st.sensor_temp_centi.store(-150); // sub-zero readings must survive the sign
        const std::string line = rep.PeriodicLine(2.0, 65);
        CAPTURE(line);
        CHECK(Contains(line, "temp=-1.5"));
        CHECK_FALSE(Contains(line, "trig="));
    }
}

TEST_CASE("stats: final_summary contains the session totals") {
    gox::CameraStats st;
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

    gox::StatsReporter rep("cam1", &st);
    const std::string s = rep.FinalSummary(/*uptime_s=*/3661);
    CAPTURE(s);
    CHECK(Contains(s, "[cam1]"));
    CHECK(Contains(s, "[Statistics] [cam1] Final:"));
    CHECK(Contains(s, "duration=01:01:01"));
    CHECK(Contains(s, "frames_ok=1000"));
    CHECK(Contains(s, "incomplete=2"));
    CHECK(Contains(s, "dropped_queue=3"));
    CHECK(Contains(s, "dropped_error=4"));
    CHECK(Contains(s, "blockid_gaps=5"));
    CHECK(Contains(s, "frames_lost=6"));
    CHECK(Contains(s, "stream_blocks_dropped=7"));
    CHECK(Contains(s, "stream_errors=8"));
    CHECK(Contains(s, "frames_written=997"));
    CHECK(Contains(s, "bytes=2.86 MiB")); // 3000000 / 2^20
    CHECK(Contains(s, "segments=2"));
    // No counter bound (freerun, or a camera without counters): no claim made.
    CHECK_FALSE(Contains(s, "triggers="));

    // With Counter0 bound, missed = triggers - everything the camera emitted
    // (1000 ok + 2 incomplete + 3 queue-dropped + 4 error-dropped + 6 lost).
    st.trigger_count.store(1020);
    const std::string with_counter = rep.FinalSummary(3661);
    CAPTURE(with_counter);
    CHECK(Contains(with_counter, "triggers=1020  missed=5"));
}

TEST_CASE("chunk_pool: acquire until exhaustion, release restores capacity") {
    gox::ChunkPool pool(3, 256);
    CHECK(pool.capacity() == 3u);
    CHECK(pool.ChunkBytes() == 256u);
    CHECK(pool.Available() == 3u);

    std::vector<gox::FrameChunkPtr> held;
    for (int i = 0; i < 3; ++i) {
        gox::FrameChunkPtr c = pool.Acquire();
        REQUIRE(c != nullptr);
        CHECK(c->capacity == 256u);
        CHECK(c->data != nullptr);
        held.push_back(std::move(c));
    }
    CHECK(pool.Available() == 0u);
    CHECK(pool.Acquire() == nullptr); // exhausted: returns nullptr, never blocks
    CHECK(pool.Acquire() == nullptr); // still exhausted

    // Release one chunk with dirty metadata; a fresh acquire gets it back
    // with the metadata reset.
    held[0]->meta.block_id = 42;
    held[0]->meta.status_flags = 0xFF;
    pool.Release(std::move(held[0]));
    CHECK(pool.Available() == 1u);
    gox::FrameChunkPtr again = pool.Acquire();
    REQUIRE(again != nullptr);
    CHECK(again->meta.block_id == 0u);
    CHECK(again->meta.status_flags == 0u);

    pool.Release(nullptr); // no-op
    CHECK(pool.Available() == 0u);

    pool.Release(std::move(again));
    pool.Release(std::move(held[1]));
    pool.Release(std::move(held[2]));
    CHECK(pool.Available() == pool.capacity()); // everything returned
    gox::FrameChunkPtr c = pool.Acquire();
    CHECK(c != nullptr);
    pool.Release(std::move(c));
}
