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
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/chunk_pool.hpp"
#include "core/config.hpp"
#include "core/frame.hpp"
#include "core/frame_queue.hpp"
#include "core/session_log.hpp"
#include "core/signal_stop.hpp"
#include "core/stats.hpp"
#include "ebus/camera_controller.hpp"

namespace jai::ebus {

	class StreamReceiver {
	public:
		StreamReceiver(uint32_t camera_index, const CameraConfig &cfg, CameraController *controller, StopController *stop,
					   EventLog *events, CameraStats *stats);
		~StreamReceiver();

		StreamReceiver(const StreamReceiver &) = delete;
		StreamReceiver &operator=(const StreamReceiver &) = delete;

		// Strict bring-up order per plan section 2:
		//   1. SetUserModeSocketRxBufferSize (must precede Open; 6.5+ rejects it
		//      afterwards), read back after Open and warn about kernel truncation
		//   2. Open(device_ip, port=0 auto, channel, local_ip when configured)
		//   3. packet size: 0 = NegotiatePacketSize with SetPacketSize(1476)
		//      fallback; explicit value with negotiate-then-1476 fallback
		//   4. SetStreamDestination(GetLocalIPAddress(), GetLocalPort(), channel)
		//   5. GevSCPD on the device when gev_scpd_ticks > 0
		//   6. receiver_tuning feature list applied to the stream parameters
		// All of this precedes StreamEnable (TLParamsLocked). Throws SdkError.
		void open();

		// After apply_config(): GetPayloadSize -> Alloc N buffers -> queue all.
		// N = stream.buffer_count, or auto = clamp(ceil(frame_rate*0.5)+8, 16,
		// 256) using the configured frame_rate (32 when no rate is configured).
		void allocate_buffers();

		// Acquisition loop (runs on the camera's acquisition thread): retrieve,
		// classify the operation result, copy to a pool chunk, requeue, push.
		// Returns when the stop flag is set (after a final <=500 ms drain pass)
		// or the stream aborts. max_frames > 0 caps recorded-OK frames and
		// requests StopReason::LimitReached when hit.
		void run_acquisition(ChunkPool &pool, BoundedQueue<FrameChunkPtr> &queue, uint64_t max_frames);

		// Ordered teardown (idempotent): StreamDisable (errors ignored when the
		// link is down) -> AbortQueuedBuffers -> retrieve-all-aborted loop ->
		// delete buffers -> Close. AcquisitionStop must already have been sent
		// and run_acquisition must have returned.
		void teardown();

		// Makes run_acquisition() exit even when no global stop was requested
		// (used by CameraSession::stop_and_join so a destructor-driven teardown
		// can never hang on the acquisition thread's join).
		void request_stop_local() { local_stop_.store(true, std::memory_order_relaxed); }

		// Mirrors PvStream GenICam statistics (read by name, missing parameters
		// silently skipped) into CameraStats.stream_*. Main-thread only.
		void poll_stream_stats();

		// Session-end full dump of all readable stream parameters.
		void dump_stream_params(const std::string &path);

		uint32_t packet_size() const { return packet_size_; }
		uint64_t expected_payload_size() const { return expected_payload_size_; }
		size_t buffer_count() const { return buffers_.size(); }

		nlohmann::ordered_json info_json() const;  // for session.json

	private:
		// Handles one retrieved buffer; requeues it before returning (except on
		// the ABORTED path). Returns false when the loop must stop.
		bool process_buffer(PvBuffer *buffer, const PvResult &op_result, ChunkPool &pool, BoundedQueue<FrameChunkPtr> &queue,
							uint64_t max_frames);

		uint32_t camera_index_;
		std::string camera_id_;
		const CameraConfig &cfg_;
		CameraController *controller_;
		StopController *stop_;
		EventLog *events_;
		CameraStats *stats_;

		std::unique_ptr<PvStreamGEV> stream_;
		std::vector<std::unique_ptr<PvBuffer>> buffers_;
		bool torn_down_ = false;
		std::atomic<bool> local_stop_{ false };

		uint32_t packet_size_ = 0;			  // GevSCPSPacketSize after negotiation
		uint64_t expected_payload_size_ = 0;  // device PayloadSize at allocation time
		uint32_t rx_buffer_requested_ = 0;
		uint32_t rx_buffer_effective_ = 0;
		std::string local_ip_;
		uint16_t local_port_ = 0;

		// acquisition-loop state
		bool have_last_bid_ = false;
		uint64_t last_bid_ = 0;
		bool have_last_dts_ = false;
		uint64_t last_dts_ = 0;
		uint64_t recorded_ok_ = 0;
		uint64_t last_behind_warn_mono_ns_ = 0;

		// last stream-statistics poll (main thread)
		int64_t stat_block_count_ = -1;
		int64_t stat_blocks_dropped_ = -1;
		int64_t stat_block_ids_missing_ = -1;
		int64_t stat_error_count_ = -1;
		double stat_acquisition_rate_ = -1.0;
		double stat_bandwidth_ = -1.0;
	};

}  // namespace jai::ebus
