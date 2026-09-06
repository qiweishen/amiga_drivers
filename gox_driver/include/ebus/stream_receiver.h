#pragma once

// GVSP data plane: owns the PvStreamGEV and a manually managed PvBuffer pool
// (no PvPipeline — buffer ownership and requeue timing stay under our
// control). The acquisition loop copies each payload into a core ChunkPool
// chunk and requeues the PvBuffer in the same iteration, so the SDK's buffer
// pool can never be starved by downstream I/O jitter.

#include <PvBuffer.h>
#include <PvStreamGEV.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "block_id_tracker.h"
#include "chunk_pool.h"
#include "app_config.h"
#include "bounded_queue.h"
#include "frame.h"
#include "signal_stop.h"
#include "stats.h"
#include "ebus/camera_controller.h"


namespace gox::ebus {
    class StreamReceiver {
    public:
        StreamReceiver(const CameraConfig &cfg, const OutputConfig &output, CameraController *controller,
                       StopController *stop, CameraStats *stats);

        ~StreamReceiver();

        StreamReceiver(const StreamReceiver &) = delete;

        StreamReceiver &operator=(const StreamReceiver &) = delete;

        // Strict bring-up order per plan section 2:
        //   1. SetUserModeSocketRxBufferSize (must precede Open in eBUS 6.5.1),
        //      read back after Open and retain both API result codes
        //   2. Open(device_ip, port=0 auto, channel 0, local_ip when configured)
        //   3. packet size: 0 = NegotiatePacketSize with SetPacketSize(1476)
        //      fallback; explicit value with negotiate-then-1476 fallback
        //   4. SetStreamDestination(GetLocalIPAddress(), GetLocalPort(), 0)
        //   5. receiver_tuning feature list applied to the stream parameters
        // No DEVICE feature is written here - their legal ranges depend on the
        // format/ROI writes that apply_config makes afterwards (manual p.130).
        // All of this precedes StreamEnable (TLParamsLocked). Throws SdkError.
        void Open();

        // After ApplyConfig(): GetPayloadSize -> Alloc N buffers -> queue all.
        // N = network.buffer_count or AutoBufferCount() (buffering.hpp), in
        // both cases clamped to the stream's GetQueuedBufferMaximum().
        void AllocateBuffers();

        // Acquisition loop (runs on the camera's acquisition thread): retrieve,
        // classify the operation result, copy to a pool chunk, requeue, push.
        // Returns when the stop flag is set (after a final <=500 ms drain pass)
        // or the stream aborts. max_frames > 0 caps recorded-OK frames and
        // requests StopReason::kLimitReached when hit. Total silence is NOT
        // judged here: it is a legitimate idle state under an external trigger,
        // so the loop only publishes stats_->data_reference_mono_ns and Main's
        // no-data watchdog decides (Guards: in config-main.yaml).
        void RunAcquisition(ChunkPool &pool, common::BoundedQueue<FrameChunkPtr> &queue, uint64_t max_frames);

        // Ordered Teardown (idempotent): StreamDisable (errors ignored when the
        // link is down) -> AbortQueuedBuffers -> retrieve-all-aborted loop ->
        // delete buffers -> Close. AcquisitionStop must already have been sent
        // and run_acquisition must have returned.
        void Teardown();

        // Makes RunAcquisition() exit even when no global stop was requested
        // (used by CameraSession::StopAndJoin so a destructor-driven teardown
        // can never hang on the acquisition thread's join).
        void RequestStopLocal() { local_stop_.store(true, std::memory_order_relaxed); }

        // Mirrors PvStream GenICam statistics (read by name, missing parameters
        // silently skipped) into CameraStats.stream_*. Main-thread only.
        void PollStreamStats();

        // Session-end full dump of all readable stream parameters.
        void DumpStreamParams(const std::string &path);

        uint32_t PacketSize() const { return packet_size_; }
        uint32_t SocketRxRequestedBytes() const { return rx_buffer_requested_; }
        uint32_t SocketRxEffectiveBytes() const { return rx_buffer_effective_; }
        const std::string &SocketRxSetResult() const { return rx_buffer_set_result_; }
        const std::string &SocketRxReadResult() const { return rx_buffer_read_result_; }
        uint64_t ExpectedPayloadSize() const { return expected_payload_size_; }
        size_t BufferCount() const { return buffers_.size(); }

    private:
        // Handles one retrieved buffer; requeues it before returning (except on
        // the ABORTED path). Returns false when the loop must stop.
        bool process_buffer(PvBuffer *buffer, const PvResult &op_result, ChunkPool &pool,
                            common::BoundedQueue<FrameChunkPtr> &queue, uint64_t max_frames);

        std::string camera_id_;
        const CameraConfig &cfg_;
        const OutputConfig &output_; // global output block (queue/drop policies)
        CameraController *controller_;
        StopController *stop_;
        CameraStats *stats_;

        std::unique_ptr<PvStreamGEV> stream_;
        std::vector<std::unique_ptr<PvBuffer> > buffers_;
        std::size_t requeue_failures_ = 0; // acquisition thread only
        bool oversize_warned_ = false;

        // QueueBuffer with retry; a buffer lost for good counts towards pool exhaustion
        void Requeue(PvBuffer *buffer);
        bool torn_down_ = false;
        std::atomic<bool> local_stop_{false};

        uint32_t packet_size_ = 0; // GevSCPSPacketSize after negotiation
        uint64_t expected_payload_size_ = 0; // device PayloadSize at allocation time
        uint32_t rx_buffer_requested_ = 0;
        uint32_t rx_buffer_effective_ = 0; // raw SDK SO_RCVBUF readback; 0 if unavailable
        std::string rx_buffer_set_result_ = "NOT_QUERIED";
        std::string rx_buffer_read_result_ = "NOT_QUERIED";
        std::string local_ip_;
        uint16_t local_port_ = 0;

        // acquisition-loop state
        BlockIdTracker block_id_;
        bool have_last_dts_ = false;
        uint64_t last_dts_ = 0;
        uint64_t recorded_ok_ = 0;
        uint64_t last_behind_warn_mono_ns_ = 0;
    };
} // namespace gox::ebus
