#pragma once

// Per-camera aggregation: one CameraSession owns the whole vertical slice —
// controller, PTP manager, stream receiver, chunk pool, frame queue,
// recorder and the acquisition + writer threads. Sessions never share
// mutable state with each other; the only cross-camera object is the
// process-wide StopController.

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "chunk_pool.h"
#include "app_config.h"
#include "bounded_queue.h"
#include "frame.h"
#include "device_json.h"
#include "recorder.h"
#include "signal_stop.h"
#include "stats.h"
#include "ebus/camera_controller.h"
#include "ebus/ptp_manager.h"
#include "ebus/stream_receiver.h"

namespace gox::ebus {
    class CameraSession {
    public:
        // `app` must outlive the session (owned by CaptureRunner); it carries
        // the global output/ptp blocks shared by every camera.
        CameraSession(const CameraConfig &cfg, const AppConfig &app, const uint8_t session_uuid[16],
                      StopController *stop);

        ~CameraSession();

        CameraSession(const CameraSession &) = delete;

        CameraSession &operator=(const CameraSession &) = delete;

        // BringUpSession() + StartStreaming() (fx10 phase split). Throws
        // on any failure with the concrete "[<id>] ..." error message.
        void Start(const std::string &session_dir);

        // Ordered shutdown; the caller must already have requested a stop:
        // AcquisitionStop -> acquisition thread drains and closes the queue ->
        // join -> stream stats dump -> stream teardown -> writer drains to
        // close-and-drained and closes the recorder -> join -> disconnect.
        // Idempotent; also cleans up partially started sessions.
        void StopAndJoin();

        const std::string &id() const { return cfg_.id; }
        CameraStats &Stats() { return stats_; }

        // Silence since this camera last handed over a buffer, microseconds, for
        // Main's no-data watchdog. nullopt when the watchdog must not apply:
        // the camera is not acquiring, or it runs on an external trigger where
        // silence only means the pulses stopped. Reads one atomic; safe from
        // any thread.
        std::optional<uint64_t> MicrosSinceLastData() const;
        StatsReporter &Reporter() { return reporter_; }
        bool Started() const { return started_; }

        // True when the capture had zero drops / incompletes / gaps (exit code 0).
        bool Clean() const;

        // Main-thread periodic hooks.
        void PollStreamStats();

        void RefreshPtpOffset();

        // PTP guard (PtpManager::CheckHealth). False = synchronization was lost
        // and the session must stop. True when PTP is off or still healthy.
        bool CheckPtpHealth();

        // One telemetry.jsonl row (temperatures, Counter0, PAUSE frames, PTP)
        // plus the CameraStats gauges the status line shows. Runs on the same
        // tick as the PTP guard and is gated the same way; never throws.
        void PollDeviceTelemetry();

    private:
        // Connect (mac discovery or direct ip) -> factory defaults (UserSetLoad
        // Default) -> PTP enable + wait -> stream open/negotiate/destination ->
        // GenICam apply -> PTP baseline
        void BringUpSession();

        // buffers -> recorder + queue/pool -> StreamEnable -> AcquisitionStart
        // -> acquisition + writer threads
        void StartStreaming();

        void AcqThreadMain();

        void WriterThreadMain();

        void TeardownDevices(); // receiver teardown + Disconnect (best effort)

        // <cam>/device.json, written once the recorder created <cam>/ and
        // before AcquisitionStart. Failures only warn: metadata must never cost
        // a recording.
        void WriteDeviceJson();

        CameraConfig cfg_; // own copy; receiver_ keeps a reference into it
        const AppConfig &app_; // global output/ptp blocks
        uint8_t session_uuid_[16];
        StopController *stop_;

        CameraStats stats_;
        StatsReporter reporter_;

        std::unique_ptr<CameraController> controller_;
        std::unique_ptr<PtpManager> ptp_;
        std::unique_ptr<StreamReceiver> receiver_;
        std::unique_ptr<ChunkPool> pool_;
        std::unique_ptr<common::BoundedQueue<FrameChunkPtr> > queue_;
        std::unique_ptr<Recorder> recorder_;

        std::thread acq_thread_;
        std::thread writer_thread_;

        std::string camera_dir_;
        bool started_ = false;
        bool stopped_ = false;

        // Bring-up results kept for the sidecars: device.json is written after
        // the recorder created <cam>/, which is later than the apply.
        FactoryLoadResult factory_load_;
        std::vector<AppliedFeature> applied_;
        RuntimeShape runtime_shape_;
        bool counter_bound_ = false;
        std::ofstream telemetry_; // <cam>/telemetry.jsonl, one JSON object per line
        bool over_temperature_ = false; // latched so the warning fires once per excursion
    };
} // namespace gox::ebus
