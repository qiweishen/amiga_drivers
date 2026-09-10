#pragma once

#include <PvDevice.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "accounting.h"
#include "app_config.h"
#include "frame.h"
#include "pixel_format.h"
#include "recording_pipeline.h"
#include <nlohmann/json.hpp>

class PvBuffer;
class PvStream;
class PvStreamGEV;
class PvDeviceGEV;


namespace fx10 {
    class TransportError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // Geometry contract for the first-frame sanity check; from CameraControl::ReadGeometry
    struct ExpectedGeometry {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t bytes_per_pixel = 2; // STORAGE bpp (after unpack), not wire bytes
        std::uint32_t payload_size = 0; // camera PayloadSize: receive capacity, may include padding
        std::string pixel_format; // [Mono12Packed | Mono12 | Mono10Packed | Mono10 | Mono8]: checked on the first frame
        bool status_line = false; // false: first frame is scanned for the 0x66BB00FF preamble
    };

    class StreamReceiver final : protected PvDeviceEventSink {
    public:
        StreamReceiver(const NetworkConfig &network, Counters &counters);

        ~StreamReceiver(); // no `override`: PvDeviceEventSink's dtor virtuality is unspecified
        StreamReceiver(const StreamReceiver &) = delete;

        StreamReceiver &operator=(const StreamReceiver &) = delete;

        // PvDevice::CreateAndConnect by device.mac (discovery match) or device.ip. A camera on a
        // foreign subnet is a TransportError naming the remedy (GUI Camera Tools > Set IP).
        void Connect(const DeviceConfig &device);

        PvDevice *Device() { return device_; } // for CameraControl; owned by this class

        // PvStreamGEV: set user-mode socket RX buffer before Open, read it back
        // afterwards, then packet size + SetStreamDestination.
        void OpenStream();

        // Allocate the SDK + owned recording pools, StreamEnable, AcquisitionStart,
        // spawn the acquisition thread. expected_fps sizes the pools (configured rate
        // in freerun; expected PPS rate under external trigger).
        // before_acquisition runs after buffer allocation, before StreamEnable:
        // persist actual runtime metadata without racing the acquisition thread.
        void Start(IFrameSink &sink, const ExpectedGeometry &expected, double expected_fps,
                   const std::function<void()> &before_acquisition = {});
        const nlohmann::json &RuntimeMetadata() const { return runtime_metadata_; }
        // Stop/join first; stream remains open until Disconnect. Never throws.
        bool DumpStreamParams(const std::filesystem::path &path) const;

        // Deterministic Stop (see file header). Safe to call from any thread, once.
        void Stop();

        // Reference phase boundary: disarm now and require SDK acknowledgement,
        // sharing the once-per-run stop flag with Stop(). Does not drain/join;
        // the owner must still call Stop() before destroying the sink.
        void StopAcquisition();

        // Close/free stream and device. stop() first if streaming.
        void Disconnect();

        bool Streaming() const { return streaming_.load(); }
        // Fatal transport condition (link loss, first-frame mismatch, unusable stream).
        bool Failed() const { return failed_.load(); }

        std::string ErrorMessage() const;

        // Machine-readable fatal cause for exit-code mapping (kNone when !Failed()).
        // kStreamUnusable: buffers keep arriving but none of them is usable
        // (persistent RetrieveBuffer errors, failed operation results or
        // non-image payloads). Deliberately NOT the same thing as total silence,
        // which is a legitimate idle state under an external trigger and is
        // judged by Main's no-data watchdog instead (lastDataReferenceUs).
        enum class FatalKind { kNone, kLinkLost, kFirstFrame, kSinkFailed, kStreamUnusable, kRecordingPipeline, kOther };

        FatalKind GetFatalKind() const { return fatal_kind_.load(); }

        // Usable frames admitted to the recording queue. Recorder's written
        // count is separate; Stop drains the accepted queue before returning.
        std::uint64_t FramesDelivered() const { return frames_delivered_.load(); }

        // Fail-fast: true once the transport saw a lost or unusable buffer (RetrieveBuffer
        // error, failed operation result, non-image payload, BlockID anomaly, requeue
        // failure). The monitor thread stops the rig on the first event.
        bool LossSeen() const { return loss_seen_.load(std::memory_order_acquire); }

        // Steady-clock micros of the last USABLE frame, or of the moment
        // acquisition started when none has arrived yet; nullopt while not
        // streaming. Feeds Main's no-data watchdog through Fx10DriverApp.
        // Stamped on arrival, BEFORE the sink runs: this measures transport
        // silence, not a stalled write pipeline (the recorder reports that
        // itself, and a slow disk must not read as a dead camera).
        std::optional<std::uint64_t> LastDataReferenceUs() const;

    private:
        void OnLinkDisconnected(PvDevice *device) override;

        void AcquisitionLoop();
        bool SubmitFrame(const FrameView &frame, std::uint64_t first_missing = 0,
                         std::uint64_t missing_count = 0,
                         RecordingPipeline::Rejection rejection = RecordingPipeline::Rejection::kNone);

        bool CheckFrame(PvBuffer &buffer);

        void LatchFatal(const std::string &message, FatalKind kind = FatalKind::kOther);

        // Abort when buffers keep arriving but none is usable (see kStreamUnusable).
        bool CheckStreamUnusable(std::chrono::steady_clock::time_point last_arrival,
                                  std::chrono::steady_clock::time_point last_frame);

        void ExecuteAcquisitionStop(); // best-effort GenICam AcquisitionStop
        void Requeue(PvBuffer *buffer); // QueueBuffer with failure accounting
        bool DrainAborted() const; // true when the stream queue is fully drained
        void FreeBuffers();

        NetworkConfig network_;
        nlohmann::json runtime_metadata_ = nlohmann::json::object();
        Counters &counters_;
        PvDevice *device_ = nullptr;
        PvDeviceGEV *device_gev_ = nullptr;
        PvStream *stream_ = nullptr;
        PvStreamGEV *stream_gev_ = nullptr;
        std::vector<std::unique_ptr<PvBuffer> > buffers_;

        ExpectedGeometry expected_;
        bool first_frame_checked_ = false;
        const PixelFormatInfo *pixel_info_ = nullptr; // points into the static format table
        std::unique_ptr<RecordingPipeline> pipeline_;
        BlockIdTracker tracker_;

        std::thread acquisition_thread_;
        std::mutex stop_mutex_;
        std::atomic<bool> stop_requested_{false};
        std::atomic<bool> streaming_{false};
        std::atomic<bool> failed_{false};
        std::atomic<bool> link_lost_{false};
        std::atomic<bool> acq_stop_sent_{false};
        std::atomic<FatalKind> fatal_kind_{FatalKind::kNone};
        std::atomic<std::uint64_t> frames_delivered_{0};
        std::atomic<bool> loss_seen_{false};
        std::atomic<std::uint64_t> last_frame_us_{0}; // 0 = none since Start()
        std::atomic<std::uint64_t> stream_start_us_{0}; // watchdog reference before the first frame
        std::size_t queue_failures_ = 0; // acquisition thread only
        mutable std::mutex error_mutex_;
        std::string error_message_;
    };
} // namespace fx10
