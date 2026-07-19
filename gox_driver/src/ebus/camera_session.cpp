#include "ebus/camera_session.hpp"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

#include "core/logger.hpp"
#include "ebus/sdk_error.hpp"

namespace jai::ebus {

	namespace {

		// mkdir -p equivalent for absolute or relative paths.
		void make_dirs(const std::string &path) {
			std::string partial;
			for (size_t i = 0; i <= path.size(); ++i) {
				if (i == path.size() || path[i] == '/') {
					if (!partial.empty() && partial != "/") {
						if (::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
							throw std::runtime_error("mkdir " + partial + ": " + std::strerror(errno));
						}
					}
					if (i < path.size()) {
						partial.push_back('/');
					}
				} else {
					partial.push_back(path[i]);
				}
			}
		}

	}  // namespace

	const char *startup_phase_name(StartupPhase phase) {
		switch (phase) {
			case StartupPhase::None:
				return "none";
			case StartupPhase::Discovery:
				return "discovery";
			case StartupPhase::Ptp:
				return "ptp";
			case StartupPhase::Apply:
				return "apply";
			case StartupPhase::Stream:
				return "stream";
			case StartupPhase::Recorder:
				return "recorder";
		}
		return "unknown";
	}

	CameraSession::CameraSession(uint32_t camera_index, const CameraConfig &cfg, const AcquisitionLimits &limits,
								 const uint8_t session_uuid[16], StopController *stop, EventLog *events) :
		camera_index_(camera_index), cfg_(cfg), limits_(limits), stop_(stop), events_(events), reporter_(cfg.id, &stats_) {
		std::memcpy(session_uuid_, session_uuid, sizeof(session_uuid_));
	}

	CameraSession::~CameraSession() {
		try {
			stop_and_join();
		} catch (const std::exception &e) {
			LOG_ERROR("[", cfg_.id, "] teardown in destructor failed: ", e.what());
		}
	}

	void CameraSession::start(const std::string &session_dir, bool validate_only) {
		camera_dir_ = session_dir + "/" + cfg_.id;

		// --- Discovery + connect. The Discovery object (and thus the PvSystem
		// owning the PvDeviceInfo) stays alive until Connect() has returned.
		try {
			Discovery discovery;
			DiscoveredDevice dev = discovery.find_camera(cfg_.selector, cfg_.discovery);
			controller_ = std::make_unique<CameraController>(cfg_.id, stop_, events_);
			controller_->connect(discovery.matched_info(), dev);
		} catch (const std::exception &e) {
			throw StartupError(StartupPhase::Discovery, "[" + cfg_.id + "] " + e.what());
		}

		// --- PTP: enable first so BMCA/servo convergence overlaps the rest of
		// the bring-up as little as the task ordering allows, then wait.
		ptp_ = std::make_unique<PtpManager>(cfg_.id, controller_->params(), cfg_.ptp, events_);
		try {
			if (!ptp_->enable()) {
				throw StartupError(StartupPhase::Ptp, "[" + cfg_.id + "] PTP enable failed (on_timeout=abort)");
			}
			if (!ptp_->wait_for_sync(stop_)) {
				throw StartupError(StartupPhase::Ptp, "[" + cfg_.id +
															  "] PTP synchronization not reached "
															  "(on_timeout=abort)");
			}
		} catch (const StartupError &) {
			throw;
		} catch (const std::exception &e) {
			throw StartupError(StartupPhase::Ptp, "[" + cfg_.id + "] " + e.what());
		}

		// --- Stream open (rx buffer, negotiate, destination, SCPD, tuning).
		receiver_ = std::make_unique<StreamReceiver>(camera_index_, cfg_, controller_.get(), stop_, events_, &stats_);
		try {
			receiver_->open();
		} catch (const std::exception &e) {
			throw StartupError(StartupPhase::Stream, "[" + cfg_.id + "] " + e.what());
		}

		// --- GenICam configuration (ordered apply engine + verification).
		try {
			controller_->apply_config(cfg_);
		} catch (const std::exception &e) {
			throw StartupError(StartupPhase::Apply, "[" + cfg_.id + "] " + e.what());
		}

		// --- Reproducibility snapshots (before StreamEnable, per plan).
		try {
			make_dirs(camera_dir_);
			controller_->write_feature_snapshot(camera_dir_ + "/genicam_snapshot.txt", camera_dir_ + "/device_config.pvcfg");
		} catch (const StartupError &) {
			throw;
		} catch (const std::exception &e) {
			throw StartupError(StartupPhase::Recorder, "[" + cfg_.id + "] " + e.what());
		}

		// First PTP offset record (session-start baseline for drift reporting).
		ptp_->refresh_offset();

		if (validate_only) {
			LOG_INFO("[", cfg_.id, "] validate-only: configuration verified; tearing down");
			teardown_devices();
			return;
		}

		// --- Buffers, recorder, queue/pool.
		try {
			receiver_->allocate_buffers();
		} catch (const std::exception &e) {
			throw StartupError(StartupPhase::Stream, "[" + cfg_.id + "] " + e.what());
		}

		RecorderOptions opts;
		opts.camera_dir = camera_dir_;
		opts.camera_id = cfg_.id;
		opts.camera_serial = controller_->identity().serial;
		std::memcpy(opts.session_uuid, session_uuid_, sizeof(opts.session_uuid));
		opts.segment_max_bytes = static_cast<uint64_t>(cfg_.recording.segment_size_gib * static_cast<double>(1ull << 30));
		opts.record_align = cfg_.recording.record_align;
		opts.payload_crc = cfg_.recording.payload_crc == "crc32c";
		opts.flush_interval_bytes = static_cast<uint64_t>(cfg_.recording.flush_interval_mb) << 20;
		opts.min_free_bytes = static_cast<uint64_t>(cfg_.recording.min_free_gib * static_cast<double>(1ull << 30));
		opts.debug_slowdown_us = cfg_.recording.debug_slowdown_us;

		// Pool = queue capacity + 2 (one chunk in the writer, one in the acquirer);
		// with fewer the pool runs dry first and "block" degrades to drop_newest.
		try {
			pool_ = std::make_unique<ChunkPool>(cfg_.recording.queue_max_frames + 2, receiver_->expected_payload_size());
			queue_ = std::make_unique<BoundedQueue<FrameChunkPtr>>(cfg_.recording.queue_max_frames);
		} catch (const std::bad_alloc &) {
			throw StartupError(StartupPhase::Stream, "[" + cfg_.id + "] cannot allocate " +
															 std::to_string(cfg_.recording.queue_max_frames + 2) +
															 " frame chunks of " + std::to_string(receiver_->expected_payload_size()) +
															 " bytes each (out of memory; lower recording.queue_max_frames)");
		}
		stats_.queue_capacity.store(queue_->capacity(), std::memory_order_relaxed);
		recorder_ = std::make_unique<Recorder>(opts, &stats_);
		try {
			recorder_->open();
		} catch (const IoError &e) {
			throw StartupError(StartupPhase::Recorder, "[" + cfg_.id + "] " + e.what());
		}

		// --- Go: TLParamsLocked, then AcquisitionStart, then the worker threads.
		try {
			controller_->stream_enable();
			controller_->acquisition_start();
		} catch (const std::exception &e) {
			throw StartupError(StartupPhase::Stream, "[" + cfg_.id + "] " + e.what());
		}
		started_ = true;
		acq_thread_ = std::thread(&CameraSession::acq_thread_main, this);
		writer_thread_ = std::thread(&CameraSession::writer_thread_main, this);
		LOG_INFO("[", cfg_.id, "] recording to ", camera_dir_);
	}

	void CameraSession::acq_thread_main() {
		try {
			receiver_->run_acquisition(*pool_, *queue_, limits_.max_frames);
		} catch (const std::exception &e) {
			LOG_ERROR("[", cfg_.id, "] acquisition thread failed: ", e.what());
			stop_->request_stop(StopReason::Error);
		}
		// EOS for the writer: close() lets pop() drain the remaining items.
		queue_->close();
	}

	void CameraSession::writer_thread_main() {
		bool io_failed = false;
		FrameChunkPtr chunk;
		while (queue_->pop(chunk)) {
			stats_.queue_depth.store(queue_->size(), std::memory_order_relaxed);
			if (!io_failed) {
				try {
					recorder_->write_frame(chunk->meta, chunk->data.get(), static_cast<size_t>(chunk->meta.payload_size));
				} catch (const IoError &e) {
					LOG_ERROR("[", cfg_.id, "] write failed: ", e.what());
					if (events_ != nullptr) {
						events_->log("io_error", nlohmann::ordered_json{ { "camera", cfg_.id }, { "error", e.what() } });
					}
					stop_->request_stop(StopReason::Error);
					io_failed = true;  // keep draining to unblock the producer
				}
			}
			pool_->release(std::move(chunk));
		}
		try {
			recorder_->close();
		} catch (const IoError &e) {
			LOG_ERROR("[", cfg_.id, "] recorder close failed: ", e.what());
			stop_->request_stop(StopReason::Error);
		}
	}

	void CameraSession::teardown_devices() {
		if (receiver_) {
			try {
				controller_->acquisition_stop(/*ignore_errors=*/true);
			} catch (const std::exception &e) {
				LOG_DEBUG("[", cfg_.id, "] acquisition stop during teardown: ", e.what());
			}
			receiver_->teardown();
		}
		if (controller_) {
			controller_->disconnect();
		}
	}

	void CameraSession::stop_and_join() {
		if (stopped_) {
			return;
		}
		stopped_ = true;

		if (!started_) {
			// validate-only or failed bring-up: devices only, no threads.
			teardown_devices();
			return;
		}

		// Ordered shutdown — the sequence is a constraint:
		// 0. force the acquisition loop to exit (a teardown must never hang on join)
		receiver_->request_stop_local();

		// 1. stop the source (errors ignored when the link is already gone)
		try {
			controller_->acquisition_stop(/*ignore_errors=*/true);
		} catch (const std::exception &e) {
			LOG_DEBUG("[", cfg_.id, "] AcquisitionStop during shutdown: ", e.what());
		}

		// 2. acquisition thread drains RetrieveBuffer, closes the queue, exits
		if (acq_thread_.joinable()) {
			acq_thread_.join();
		}

		// 3. final stream statistics (needs the open stream), then stream teardown
		receiver_->poll_stream_stats();
		receiver_->dump_stream_params(camera_dir_ + "/stream_stats.txt");
		receiver_->teardown();

		// 4. writer drains the closed queue and closes the recorder
		if (writer_thread_.joinable()) {
			writer_thread_.join();
		}

		// 5. control channel down last
		controller_->disconnect();
		LOG_INFO("[", cfg_.id, "] session stopped");
	}

	bool CameraSession::clean() const {
		const CameraStats::Snapshot s = stats_.snapshot();
		return s.frames_incomplete == 0 && s.frames_error_dropped == 0 && s.frames_dropped_queue == 0 && s.frames_lost_gap == 0 &&
			   s.stream_blocks_dropped == 0;
	}

	void CameraSession::poll_stream_stats() {
		if (started_ && !stopped_ && receiver_) {
			receiver_->poll_stream_stats();
		}
	}

	void CameraSession::refresh_ptp_offset() {
		if (started_ && !stopped_ && ptp_ && controller_ && controller_->connected()) {
			ptp_->refresh_offset();
		}
	}

	nlohmann::ordered_json CameraSession::summary_json() const {
		nlohmann::ordered_json j;
		j["id"] = cfg_.id;
		if (controller_) {
			const DiscoveredDevice &d = controller_->identity();
			j["device"] = nlohmann::ordered_json{
				{ "model", d.model }, { "vendor", d.vendor }, { "serial", d.serial },		{ "firmware", d.firmware },
				{ "mac", d.mac },	  { "ip", d.ip },		  { "user_name", d.user_name },
			};
			j["applied_features"] = controller_->applied_json();
			j["timestamp_tick_frequency"] = controller_->timestamp_tick_frequency();
		}
		if (ptp_) {
			j["ptp"] = ptp_->report().to_json();
		}
		if (receiver_) {
			j["stream"] = receiver_->info_json();
		}
		const CameraStats::Snapshot s = stats_.snapshot();
		j["stats"] = nlohmann::ordered_json{
			{ "frames_retrieved_ok", s.frames_retrieved_ok },
			{ "frames_incomplete", s.frames_incomplete },
			{ "frames_error_dropped", s.frames_error_dropped },
			{ "frames_dropped_queue", s.frames_dropped_queue },
			{ "blockid_gap_events", s.blockid_gap_events },
			{ "frames_lost_gap", s.frames_lost_gap },
			{ "retrieve_timeouts", s.retrieve_timeouts },
			{ "frames_written", s.frames_written },
			{ "bytes_written", s.bytes_written },
			{ "segments_created", s.segments_created },
			{ "stream_blocks_dropped", s.stream_blocks_dropped },
			{ "stream_error_count", s.stream_error_count },
		};
		j["clean"] = clean();
		return j;
	}

}  // namespace jai::ebus
