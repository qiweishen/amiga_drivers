#pragma once

#include <PvDevice.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "accounting.hpp"
#include "app_config.hpp"
#include "frame.hpp"
#include "pixel_format.hpp"

class PvBuffer;
class PvStream;
class PvStreamGEV;
class PvDeviceGEV;


namespace fx10 {
    class TransportError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct DiscoveredDevice {
        std::string display_id;
        std::string connection_id;
        std::string ip;
        std::string mac;
        bool configuration_valid = true; // false: device IP unreachable from its NIC's subnet
    };

    std::vector<DiscoveredDevice> discoverDevices(std::chrono::milliseconds timeout);

    // Geometry contract for the first-frame sanity check; from CameraControl::readGeometry
    struct ExpectedGeometry {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t bytes_per_pixel = 2; // STORAGE bpp (after unpack), not wire bytes
        std::uint32_t payload_size = 0; // wire bytes per frame (camera PayloadSize; packed formats < W*H*bpp)
        std::string pixel_format; // [Mono12Packed | Mono12 | Mono10Packed | Mono10 | Mono8]: checked on the first frame
        bool status_line = false; // false: first frame is scanned for the 0x66BB00FF preamble
    };

    class StreamReceiver final : protected PvDeviceEventSink {
    public:
        StreamReceiver(const NetworkConfig &network, Counters &counters);

        ~StreamReceiver(); // no `override`: PvDeviceEventSink's dtor virtuality is unspecified
        StreamReceiver(const StreamReceiver &) = delete;

        StreamReceiver &operator=(const StreamReceiver &) = delete;

        // PvDevice::CreateAndConnect by device.id (preferred), device.mac
        // (discovery match + optional FORCEIP rescue) or device.ip.
        void connect(const DeviceConfig &device);

        PvDevice *device() { return device_; } // for CameraControl; owned by this class

        // PvStream::CreateAndOpen + packet size (negotiate, or GevSCPSPacketSize when
        // network.packet_size > 0) + SetStreamDestination + user-mode socket RX buffer.
        void openStream();

        // Allocate/queue the pool (planBufferPool), StreamEnable, AcquisitionStart,
        // spawn the acquisition thread. expected_fps sizes the pool (configured rate
        // in freerun; expected PPS rate under external trigger).
        void start(IFrameSink &sink, const ExpectedGeometry &expected, double expected_fps,
                   const WatchdogConfig &watchdog, double no_frame_abort_s);

        // Deterministic stop (see file header). Safe to call from any thread, once.
        void stop();

        // Close/free stream and device. stop() first if streaming.
        void disconnect();

        bool streaming() const { return streaming_.load(); }
        // Fatal transport condition (link loss, first-frame mismatch, watchdog abort).
        bool failed() const { return failed_.load(); }

        std::string errorMessage() const;

        // Machine-readable fatal cause for exit-code mapping (kNone when !failed()).
        enum class FatalKind { kNone, kLinkLost, kWatchdog, kFirstFrame, kSinkFailed, kOther };

        FatalKind fatalKind() const { return fatal_kind_.load(); }

        std::uint64_t framesDelivered() const { return frames_delivered_.load(); }

    private:
        void OnLinkDisconnected(PvDevice *device) override;

        void acquisitionLoop_(IFrameSink &sink);

        bool checkFirstFrame_(PvBuffer &buffer);
        void warnIfUnexpectedStatusLine_(const std::uint8_t *data); // canonical (unpacked) layout

        void latchFatal_(const std::string &message, FatalKind kind = FatalKind::kOther);

        void executeAcquisitionStop_(); // best-effort GenICam AcquisitionStop
        void requeue_(PvBuffer *buffer); // QueueBuffer with failure accounting
        bool drainAborted_(); // true when the stream queue is fully drained
        void freeBuffers_();

        NetworkConfig network_;
        Counters &counters_;
        std::string connection_target_;

        PvDevice *device_ = nullptr;
        PvDeviceGEV *device_gev_ = nullptr;
        PvStream *stream_ = nullptr;
        PvStreamGEV *stream_gev_ = nullptr;
        std::vector<std::unique_ptr<PvBuffer> > buffers_;

        ExpectedGeometry expected_;
        WatchdogConfig watchdog_;
        double no_frame_abort_s_ = 0.0;
        bool first_frame_checked_ = false;
        bool preamble_checked_ = false; // one-time status-line warn on canonical data
        // Packed wire formats are unpacked here into the canonical uint16 layout
        // before the sink ever sees the frame (acquisition thread only)
        const PixelFormatInfo *pixel_info_ = nullptr; // points into the static format table
        std::vector<std::uint16_t> unpack_buf_;
        BlockIdTracker tracker_;

        std::thread acquisition_thread_;
        std::atomic<bool> stop_requested_{false};
        std::atomic<bool> streaming_{false};
        std::atomic<bool> failed_{false};
        std::atomic<bool> link_lost_{false};
        std::atomic<bool> acq_stop_sent_{false};
        std::atomic<FatalKind> fatal_kind_{FatalKind::kNone};
        std::atomic<std::uint64_t> frames_delivered_{0};
        std::size_t queue_failures_ = 0; // acquisition thread only
        bool buffers_deferred_ = false; // free deferred until after stream Close
        mutable std::mutex error_mutex_;
        std::string error_message_;
    };
} // namespace fx10
