// jai_fake_capture: SDK-free end-to-end exerciser for the core storage chain.
//
// Synthesizes deterministic camera frames and drives them through the exact
// production pipeline — ChunkPool -> BoundedQueue -> Recorder — with the same
// drop_newest backpressure policy as the real driver, but with no eBUS SDK
// and no camera. Run it inside the Docker container to validate the disk path
// (segment layout, rotation, index files, sustained throughput) before the
// SDK is even installed, then check the output with scripts/inspect_raw.py.
//
// Payload pattern: byte(x, y, frame) = (x + y + frame) & 0xFF, so inspection
// tools can verify payload integrity without any stored reference data.

#include "core/chunk_pool.hpp"
#include "core/frame.hpp"
#include "core/frame_queue.hpp"
#include "core/logger.hpp"
#include "core/recorder.hpp"
#include "core/signal_stop.hpp"
#include "core/stats.hpp"
#include "core/util.hpp"
#include "version.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <sys/statvfs.h>

namespace {

constexpr uint32_t kPfncMono8 = 0x01080001u; // PFNC code recorded in every frame header

struct Options {
    std::string out_dir = "./fake_capture_out";
    uint64_t frames = 500;
    uint32_t width = 640;
    uint32_t height = 480;
    double fps = 100.0; // 0 = unpaced (as fast as the writer allows)
    uint32_t queue_capacity = 32;
    uint32_t pool_size = 32;
    uint64_t segment_mib = 256;
    uint32_t record_align = 4096;
    bool payload_crc = false;
    uint32_t slowdown_us = 0; // Recorder debug hook: writer sleeps per frame
};

void print_usage(const char* argv0) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "SDK-free end-to-end exerciser for the raw-capture storage chain. Synthesizes\n"
        "Mono8 frames through the production pipeline (ChunkPool -> BoundedQueue ->\n"
        "Recorder) with no camera and no eBUS SDK, writing a real session layout:\n"
        "  {out}/cam0/seg_NNNNN.raw + seg_NNNNN.idx.jsonl + segments.jsonl\n"
        "\n"
        "Options:\n"
        "  --out <dir>        output directory (default ./fake_capture_out)\n"
        "  --frames <n>       frames to synthesize (default 500)\n"
        "  --width <px>       frame width in pixels (default 640)\n"
        "  --height <px>      frame height in pixels (default 480)\n"
        "  --fps <hz>         producer pacing; 0 = as fast as possible (default 100)\n"
        "  --queue <n>        frame queue capacity, >= 2 (default 32)\n"
        "  --pool <n>         chunk pool size, >= 2 (default 32)\n"
        "  --segment-mib <n>  segment rotation threshold in MiB (default 256)\n"
        "  --align <n>        record alignment, power of two; 1 disables (default 4096)\n"
        "  --payload-crc      store a CRC-32C of every payload in its frame header\n"
        "  --slowdown-us <n>  writer sleeps n microseconds per frame (forces drops)\n"
        "  -h, --help         show this help and exit\n"
        "\n"
        "Payload pattern: byte(x, y, frame) = (x + y + frame) & 0xFF.\n"
        "Exit code 0 when every frame was written with zero drops, 1 otherwise.\n",
        argv0);
}

bool parse_u64(const char* s, uint64_t& out) {
    if (s == nullptr || *s == '\0' || std::strchr(s, '-') != nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

bool parse_u32(const char* s, uint32_t& out) {
    uint64_t v = 0;
    if (!parse_u64(s, v) || v > UINT32_MAX) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool parse_fps(const char* s, double& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !(v >= 0.0) || v > 1e6) {
        return false;
    }
    out = v;
    return true;
}

// Returns -1 to proceed, otherwise the exit code (0 for --help, 2 on errors).
int parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--payload-crc") {
            opts.payload_crc = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "error: %s requires a value (see --help)\n", arg.c_str());
            return 2;
        }
        const char* val = argv[++i];
        bool ok = true;
        if (arg == "--out") {
            opts.out_dir = val;
        } else if (arg == "--frames") {
            ok = parse_u64(val, opts.frames);
        } else if (arg == "--width") {
            ok = parse_u32(val, opts.width);
        } else if (arg == "--height") {
            ok = parse_u32(val, opts.height);
        } else if (arg == "--fps") {
            ok = parse_fps(val, opts.fps);
        } else if (arg == "--queue") {
            ok = parse_u32(val, opts.queue_capacity);
        } else if (arg == "--pool") {
            ok = parse_u32(val, opts.pool_size);
        } else if (arg == "--segment-mib") {
            ok = parse_u64(val, opts.segment_mib);
        } else if (arg == "--align") {
            ok = parse_u32(val, opts.record_align);
        } else if (arg == "--slowdown-us") {
            ok = parse_u32(val, opts.slowdown_us);
        } else {
            std::fprintf(stderr, "error: unknown option \"%s\" (see --help)\n", arg.c_str());
            return 2;
        }
        if (!ok) {
            std::fprintf(stderr, "error: invalid value \"%s\" for %s\n", val, arg.c_str());
            return 2;
        }
    }

    const char* problem = nullptr;
    if (opts.frames == 0) {
        problem = "--frames must be >= 1";
    } else if (opts.width == 0 || opts.height == 0) {
        problem = "--width and --height must be >= 1";
    } else if (opts.queue_capacity < 2) {
        problem = "--queue must be >= 2";
    } else if (opts.pool_size < 2) {
        problem = "--pool must be >= 2";
    } else if (opts.segment_mib == 0) {
        problem = "--segment-mib must be >= 1";
    } else if (opts.record_align == 0 || (opts.record_align & (opts.record_align - 1)) != 0) {
        problem = "--align must be a power of two (1 disables alignment)";
    }
    if (problem != nullptr) {
        std::fprintf(stderr, "error: %s\n", problem);
        return 2;
    }
    return -1;
}

uint64_t free_disk_bytes(const std::string& path) {
    struct statvfs vfs {};
    if (::statvfs(path.c_str(), &vfs) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    const int early = parse_args(argc, argv, opts);
    if (early >= 0) {
        return early;
    }

    jai::logger().set_level(jai::LogLevel::Info);

    const size_t payload_bytes = static_cast<size_t>(opts.width) * opts.height; // Mono8: 1 B/px
    // Fake camera clock: device_ts advances by exactly 1e9/fps ns per frame
    // (a nominal 1 ms when unpaced), starting from the host wall clock as a
    // stand-in for the PTP epoch.
    const uint64_t device_period_ns =
        opts.fps > 0.0 ? static_cast<uint64_t>(std::llround(1e9 / opts.fps)) : 1'000'000ull;

    std::error_code ec;
    std::filesystem::create_directories(opts.out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create output dir %s: %s\n", opts.out_dir.c_str(),
                     ec.message().c_str());
        return 1;
    }

    uint8_t uuid[16];
    jai::gen_uuid_v4(uuid);

    jai::RecorderOptions ropts;
    ropts.camera_dir = opts.out_dir + "/cam0";
    ropts.camera_id = "cam0";
    ropts.camera_serial = "FAKE-SYNTH";
    std::memcpy(ropts.session_uuid, uuid, 16);
    ropts.segment_max_bytes = opts.segment_mib << 20;
    ropts.record_align = opts.record_align;
    ropts.payload_crc = opts.payload_crc;
    ropts.debug_slowdown_us = opts.slowdown_us;

    std::printf("jai_fake_capture %s (git %s) - SDK-free storage pipeline exerciser\n",
                jai::version::kDriverVersion, jai::version::kGitSha);
    std::printf("  output     : %s\n", ropts.camera_dir.c_str());
    std::printf("  frames     : %" PRIu64 " x %ux%u Mono8 (%s payload/frame)\n", opts.frames,
                opts.width, opts.height, jai::human_bytes(payload_bytes).c_str());
    if (opts.fps > 0.0) {
        const uint64_t nominal = static_cast<uint64_t>(static_cast<double>(payload_bytes) * opts.fps);
        std::printf("  pacing     : %.1f fps (%s/s nominal)\n", opts.fps,
                    jai::human_bytes(nominal).c_str());
    } else {
        std::printf("  pacing     : unpaced (as fast as the writer allows)\n");
    }
    std::printf("  queue/pool : %u frames / %u chunks (%s buffered)\n", opts.queue_capacity,
                opts.pool_size,
                jai::human_bytes(static_cast<uint64_t>(opts.pool_size) * payload_bytes).c_str());
    std::printf("  segments   : %" PRIu64 " MiB max, align %u, payload_crc %s\n", opts.segment_mib,
                opts.record_align, opts.payload_crc ? "on" : "off");
    if (opts.slowdown_us > 0) {
        std::printf("  slowdown   : writer sleeps %u us/frame (drop-test hook)\n", opts.slowdown_us);
    }
    std::printf("  session    : %s\n", jai::uuid_to_string(uuid).c_str());
    std::fflush(stdout);

    jai::StopController stop;
    jai::install_signal_handlers(&stop); // Ctrl+C stops gracefully, twice force-exits

    jai::CameraStats stats;
    stats.queue_capacity.store(opts.queue_capacity, std::memory_order_relaxed);
    jai::ChunkPool pool(opts.pool_size, payload_bytes);
    jai::BoundedQueue<jai::FrameChunkPtr> queue(opts.queue_capacity);
    jai::Recorder recorder(ropts, &stats);
    try {
        recorder.open();
    } catch (const jai::IoError& e) {
        LOG_ERROR("fake_capture: cannot open recorder: ", e.what());
        return 1;
    }

    std::atomic<bool> io_failed{false};
    std::atomic<bool> writer_done{false};
    std::atomic<uint64_t> frames_produced{0};

    // Producer: stands in for the acquisition thread. Same drop_newest policy
    // as the real driver — a full pool or queue drops the new frame and moves
    // on; it never blocks the (virtual) camera.
    std::thread producer([&] {
        const uint64_t device_t0 = jai::now_realtime_ns(); // fake PTP epoch
        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < opts.frames; ++i) {
            if (stop.stop_requested()) {
                break;
            }
            jai::FrameChunkPtr chunk = pool.acquire();
            stats.frames_retrieved_ok.fetch_add(1, std::memory_order_relaxed);
            if (!chunk) {
                // Pool exhausted: every buffer is queued or being written.
                stats.frames_dropped_queue.fetch_add(1, std::memory_order_relaxed);
            } else {
                uint8_t* p = chunk->data.get();
                for (uint32_t y = 0; y < opts.height; ++y) {
                    uint8_t* row = p + static_cast<size_t>(y) * opts.width;
                    for (uint32_t x = 0; x < opts.width; ++x) {
                        row[x] = static_cast<uint8_t>(x + y + i);
                    }
                }
                jai::FrameMeta& m = chunk->meta;
                m.camera_index = 0;
                m.block_id = i + 1; // GVSP BlockIDs start at 1
                m.device_ts_ns = device_t0 + i * device_period_ns;
                m.host_realtime_ns = jai::now_realtime_ns();
                m.host_monotonic_ns = jai::now_monotonic_ns();
                m.pixel_format = kPfncMono8;
                m.width = opts.width;
                m.height = opts.height;
                m.payload_size = payload_bytes;
                m.expected_size = payload_bytes;
                if (!queue.try_push(std::move(chunk))) {
                    // A failed try_push leaves ownership with the caller.
                    stats.frames_dropped_queue.fetch_add(1, std::memory_order_relaxed);
                    pool.release(std::move(chunk));
                }
            }
            frames_produced.fetch_add(1, std::memory_order_relaxed);
            stats.queue_depth.store(queue.size(), std::memory_order_relaxed);
            if (opts.fps > 0.0) {
                std::this_thread::sleep_until(
                    t0 + std::chrono::nanoseconds(static_cast<int64_t>((i + 1) * device_period_ns)));
            }
        }
        queue.close(); // EOS for the writer
    });

    // Writer: the exact production loop — pop, write, release to the pool.
    std::thread writer([&] {
        jai::FrameChunkPtr chunk;
        while (queue.pop(chunk)) {
            if (!io_failed.load(std::memory_order_relaxed)) {
                try {
                    recorder.write_frame(chunk->meta, chunk->data.get(),
                                         static_cast<size_t>(chunk->meta.payload_size));
                } catch (const jai::IoError& e) {
                    LOG_ERROR("fake_capture: write failed: ", e.what());
                    io_failed.store(true, std::memory_order_relaxed);
                    stop.request_stop(jai::StopReason::Error);
                }
            }
            pool.release(std::move(chunk));
            stats.queue_depth.store(queue.size(), std::memory_order_relaxed);
        }
        writer_done.store(true, std::memory_order_relaxed);
    });

    // Main thread: one StatsReporter line per second until the writer drains.
    jai::StatsReporter reporter("cam0", &stats);
    const uint64_t start_mono = jai::now_monotonic_ns();
    auto last_print = std::chrono::steady_clock::now();
    while (!writer_done.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = std::chrono::steady_clock::now();
        if (now - last_print >= std::chrono::seconds(1)) {
            const double interval = std::chrono::duration<double>(now - last_print).count();
            last_print = now;
            const uint64_t uptime_s = (jai::now_monotonic_ns() - start_mono) / 1'000'000'000ull;
            std::printf(
                "%s\n",
                reporter.periodic_line(interval, uptime_s, free_disk_bytes(opts.out_dir)).c_str());
            std::fflush(stdout);
        }
    }
    producer.join();
    writer.join();

    try {
        recorder.close();
    } catch (const jai::IoError& e) {
        LOG_ERROR("fake_capture: close failed: ", e.what());
        io_failed.store(true, std::memory_order_relaxed);
    }

    const uint64_t uptime_s = (jai::now_monotonic_ns() - start_mono) / 1'000'000'000ull;
    const jai::CameraStats::Snapshot s = stats.snapshot();
    std::printf("%s\n", reporter.final_summary(uptime_s).c_str());

    const bool interrupted = stop.reason() == jai::StopReason::Signal;
    const bool pass = s.frames_dropped_queue == 0 && !io_failed.load();
    std::printf("RESULT: %s - produced=%" PRIu64 " written=%" PRIu64 " dropped=%" PRIu64
                " segments=%" PRIu64 " bytes=%s%s%s\n",
                pass ? "PASS" : "FAIL", frames_produced.load(), s.frames_written,
                s.frames_dropped_queue, s.segments_created, jai::human_bytes(s.bytes_written).c_str(),
                interrupted ? " (interrupted by signal)" : "", io_failed.load() ? " (I/O error)" : "");
    std::printf("verify with: python3 scripts/inspect_raw.py verify %s\n", ropts.camera_dir.c_str());
    return pass ? 0 : 1;
}
