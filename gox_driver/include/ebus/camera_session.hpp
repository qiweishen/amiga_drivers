#pragma once

// Per-camera aggregation: one CameraSession owns the whole vertical slice —
// controller, PTP manager, stream receiver, chunk pool, frame queue,
// recorder and the acquisition + writer threads. Sessions never share
// mutable state with each other; the only cross-camera object is the
// process-wide StopController.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "chunk_pool.hpp"
#include "app_config.hpp"
#include "frame.hpp"
#include "frame_queue.hpp"
#include "recorder.hpp"
#include "signal_stop.hpp"
#include "stats.hpp"
#include "ebus/camera_controller.hpp"
#include "ebus/ptp_manager.hpp"
#include "ebus/stream_receiver.hpp"

namespace jai::ebus {
    class CameraSession {
    public:
        // `app` must outlive the session (owned by CaptureRunner); it carries
        // the global output/disk/ptp blocks shared by every camera.
        CameraSession(uint32_t camera_index, const CameraConfig &cfg, const AppConfig &app,
                      const uint8_t session_uuid[16], StopController *stop);

        ~CameraSession();

        CameraSession(const CameraSession &) = delete;

        CameraSession &operator=(const CameraSession &) = delete;

        // bring_up_session_() + start_streaming_() (fx10 phase split). Throws
        // on any failure with the concrete "[<id>] ..." error message.
        void start(const std::string &session_dir);

        // Ordered shutdown; the caller must already have requested a stop:
        // AcquisitionStop -> acquisition thread drains and closes the queue ->
        // join -> stream stats dump -> stream teardown -> writer drains to
        // close-and-drained and closes the recorder -> join -> disconnect.
        // Idempotent; also cleans up partially started sessions.
        void stop_and_join();

        const std::string &id() const { return cfg_.id; }
        CameraStats &stats() { return stats_; }
        StatsReporter &reporter() { return reporter_; }
        bool started() const { return started_; }

        // True when the capture had zero drops / incompletes / gaps (exit code 0).
        bool clean() const;

        // Main-thread periodic hooks.
        void poll_stream_stats();

        void refresh_ptp_offset();

    private:
        // connect (mac discovery or direct ip) -> PTP enable + wait ->
        // stream open/negotiate/destination -> GenICam apply -> PTP baseline
        void bring_up_session_();

        // buffers -> recorder + queue/pool -> StreamEnable -> AcquisitionStart
        // -> acquisition + writer threads
        void start_streaming_();

        void acq_thread_main();

        void writer_thread_main();

        void teardown_devices(); // receiver teardown + disconnect (best effort)

        uint32_t camera_index_;
        CameraConfig cfg_; // own copy; receiver_ keeps a reference into it
        const AppConfig &app_; // global output/disk/ptp blocks
        uint8_t session_uuid_[16];
        StopController *stop_;

        CameraStats stats_;
        StatsReporter reporter_;

        std::unique_ptr<CameraController> controller_;
        std::unique_ptr<PtpManager> ptp_;
        std::unique_ptr<StreamReceiver> receiver_;
        std::unique_ptr<ChunkPool> pool_;
        std::unique_ptr<BoundedQueue<FrameChunkPtr> > queue_;
        std::unique_ptr<Recorder> recorder_;

        std::thread acq_thread_;
        std::thread writer_thread_;

        std::string camera_dir_;
        bool started_ = false;
        bool stopped_ = false;
    };
} // namespace jai::ebus
