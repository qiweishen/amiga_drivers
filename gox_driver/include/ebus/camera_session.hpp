#pragma once

// Per-camera aggregation: one CameraSession owns the whole vertical slice —
// controller, PTP manager, stream receiver, chunk pool, frame queue,
// recorder and the acquisition + writer threads. Sessions never share
// mutable state with each other; the only cross-camera objects are the
// process-wide StopController and the (thread-safe) EventLog.

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/chunk_pool.hpp"
#include "core/config.hpp"
#include "core/frame.hpp"
#include "core/frame_queue.hpp"
#include "core/recorder.hpp"
#include "core/session_log.hpp"
#include "core/signal_stop.hpp"
#include "core/stats.hpp"
#include "ebus/camera_controller.hpp"
#include "ebus/ptp_manager.hpp"
#include "ebus/stream_receiver.hpp"

namespace jai::ebus {

	// Which bring-up phase failed; App maps this to the documented exit codes
	// (Discovery/Apply -> 3, Ptp -> 4, Stream -> 5, Recorder -> 6).
	enum class StartupPhase { None, Discovery, Ptp, Apply, Stream, Recorder };

	const char *startup_phase_name(StartupPhase phase);

	class StartupError : public std::runtime_error {
	public:
		StartupError(StartupPhase phase, const std::string &what) : std::runtime_error(what), phase_(phase) {}

		StartupPhase phase() const { return phase_; }

	private:
		StartupPhase phase_;
	};

	class CameraSession {
	public:
		CameraSession(uint32_t camera_index, const CameraConfig &cfg, const AcquisitionLimits &limits, const uint8_t session_uuid[16],
					  StopController *stop, EventLog *events);
		~CameraSession();

		CameraSession(const CameraSession &) = delete;
		CameraSession &operator=(const CameraSession &) = delete;

		// Full bring-up: discovery -> connect (+ communication params, link
		// sink) -> PTP enable + wait -> stream open/negotiate/destination ->
		// GenICam apply -> snapshots -> buffers -> recorder -> StreamEnable ->
		// AcquisitionStart -> threads. With validate_only everything up to and
		// including the snapshots runs, then the session tears down cleanly and
		// no acquisition starts. Throws StartupError (phase-tagged).
		void start(const std::string &session_dir, bool validate_only);

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

		// Per-camera block for session.json: identity, applied features, PTP
		// report, stream info, final counters.
		nlohmann::ordered_json summary_json() const;

	private:
		void acq_thread_main();
		void writer_thread_main();
		void teardown_devices();  // receiver teardown + disconnect (best effort)

		uint32_t camera_index_;
		CameraConfig cfg_;		  // own copy; receiver_ keeps a reference into it
		AcquisitionLimits limits_;
		uint8_t session_uuid_[16];
		StopController *stop_;
		EventLog *events_;

		CameraStats stats_;
		StatsReporter reporter_;

		std::unique_ptr<CameraController> controller_;
		std::unique_ptr<PtpManager> ptp_;
		std::unique_ptr<StreamReceiver> receiver_;
		std::unique_ptr<ChunkPool> pool_;
		std::unique_ptr<BoundedQueue<FrameChunkPtr>> queue_;
		std::unique_ptr<Recorder> recorder_;

		std::thread acq_thread_;
		std::thread writer_thread_;

		std::string camera_dir_;
		bool started_ = false;
		bool stopped_ = false;
	};

}  // namespace jai::ebus
