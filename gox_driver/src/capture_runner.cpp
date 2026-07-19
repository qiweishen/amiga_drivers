#include "capture_runner.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>

#include "core/logger.hpp"
#include "core/stats.hpp"
#include "core/util.hpp"
#include "ebus/camera_session.hpp"
#include "ebus/preflight.hpp"
#include "version.hpp"

namespace jai {

	namespace {

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

		uint64_t free_disk_bytes(const std::string &path) {
			struct statvfs vfs{};
			if (::statvfs(path.c_str(), &vfs) != 0) {
				return 0;
			}
			return static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
		}

		int exit_code_for_phase(ebus::StartupPhase phase) {
			switch (phase) {
				case ebus::StartupPhase::Discovery:
				case ebus::StartupPhase::Apply:
					return 3;  // device found/connected/configured — control-plane failure
				case ebus::StartupPhase::Ptp:
					return 4;
				case ebus::StartupPhase::Stream:
					return 5;
				case ebus::StartupPhase::Recorder:
					return 6;  // I/O
				case ebus::StartupPhase::None:
					break;
			}
			return 6;
		}

		std::string basename_of(const std::string &path) {
			const size_t pos = path.find_last_of('/');
			return pos == std::string::npos ? path : path.substr(pos + 1);
		}

	}  // namespace

	CaptureRunner::CaptureRunner(AppConfig cfg, StopController *stop) : cfg_(std::move(cfg)), stop_(stop) {}

	CaptureRunner::~CaptureRunner() {
		if (initialized_ && !shutdown_done_ && !stop_->stop_requested()) {
			// Reached only via stack unwinding: an exception escaped between
			// init() and shutdown() (e.g. out of the poll loop). Record the
			// session as failed so session.json agrees with the fatal exit path.
			stop_->request_stop(StopReason::Error);
		}
		try {
			shutdown();
		} catch (...) {
			// Never throw out of a destructor (may run during unwinding).
		}
	}

	bool CaptureRunner::init(const std::string &session_dir_override, bool validate_only) {
		validate_only_ = validate_only;

		// Session directory + metadata skeleton.
		gen_uuid_v4(session_uuid_);
		start_rt_ = now_realtime_ns();
		start_mono_ = now_monotonic_ns();
		if (session_dir_override.empty()) {
			session_name_ = cfg_.recording.session_name;
			if (session_name_.empty() || session_name_ == "auto") {
				session_name_ = compact_utc(start_rt_) + "_" + hex_prefix(session_uuid_, 3);
			}
			session_dir_ = cfg_.recording.output_dir + "/" + session_name_;
		} else {
			// Unified mode: the host application owns the output layout.
			session_dir_ = session_dir_override;
			session_name_ = basename_of(session_dir_override);
		}
		session_json_path_ = session_dir_ + "/session.json";
		try {
			make_dirs(session_dir_);
		} catch (const std::exception &e) {
			LOG_ERROR("cannot create session directory: ", e.what());
			exit_code_ = 6;
			return false;
		}
		LOG_INFO("gox driver ", version::kDriverVersion, " (", version::kGitSha, ") session ", session_name_,
				 validate_only ? " [validate-only]" : "");

		if (!events_.open(session_dir_ + "/events.jsonl")) {
			LOG_WARN("cannot open events.jsonl; events will not be persisted");
		}
		events_.log("session_start", nlohmann::ordered_json{ { "session", session_name_ }, { "validate_only", validate_only } });

		doc_["format"] = nlohmann::ordered_json{ { "name", "jai-raw-seg" },
												 { "version", "1.0" },
												 { "payload_layout", "gvsp_packed_no_line_padding" } };
		doc_["session"] = nlohmann::ordered_json{ { "name", session_name_ },
												  { "uuid", uuid_to_string(session_uuid_) },
												  { "started", iso8601_utc(start_rt_) },
												  { "status", validate_only ? "validating" : "recording" } };
		doc_["driver"] = nlohmann::ordered_json{ { "version", version::kDriverVersion },
												 { "git_sha", version::kGitSha },
												 { "ebus_sdk_root", version::kEbusSdkRoot },
												 { "genicam_root", version::kGenicamRoot },
												 { "genicam_env", version::kGenicamEnvName } };
		doc_["config"] = cfg_.raw;

		// Preflight (network self-checks), then the initial session.json.
		ebus::PreflightReport preflight = ebus::run_preflight(cfg_);
		preflight.log();
		preflight_had_error_ = preflight.has_error();
		doc_["preflight"] = preflight.to_json();
		try {
			write_json_atomic(session_json_path_, doc_);
		} catch (const std::exception &e) {
			LOG_ERROR("cannot write session.json: ", e.what());
			exit_code_ = 6;
			return false;
		}
		if (preflight_had_error_ && cfg_.preflight.fail_on_error) {
			LOG_ERROR("preflight reported errors and preflight.fail_on_error is set; aborting");
			finalize("failed", "preflight errors", false);
			exit_code_ = 5;
			return false;
		}

		// Bring the cameras up.
		for (size_t i = 0; i < cfg_.cameras.size(); ++i) {
			if (cfg_.cameras[i].enabled) {
				sessions_.push_back(std::make_unique<ebus::CameraSession>(static_cast<uint32_t>(i), cfg_.cameras[i], cfg_.acquisition,
																		  session_uuid_, stop_, &events_));
			}
		}
		for (auto &session: sessions_) {
			try {
				session->start(session_dir_, validate_only);
			} catch (const std::exception &e) {
				// A stop that landed mid-bring-up (Ctrl+C during a PTP wait, or a
				// running camera's writer failing) surfaces as a StartupError of
				// whatever phase was active — report the true cause, not the phase.
				const bool interrupted = stop_->stop_requested() &&
										 (stop_->reason() == StopReason::Signal || stop_->reason() == StopReason::External);
				const bool prior_error = stop_->stop_requested() && stop_->reason() == StopReason::Error;
				const auto *startup = dynamic_cast<const ebus::StartupError *>(&e);
				if (startup != nullptr) {
					LOG_ERROR("startup failed in phase ", ebus::startup_phase_name(startup->phase()), ": ", e.what());
				} else {
					LOG_ERROR("startup failed: ", e.what());
				}
				stop_->request_stop(StopReason::Error);
				for (auto &s: sessions_) {
					s->stop_and_join();
				}
				finalize("failed", e.what(), true);
				if (interrupted) {
					exit_code_ = 130;
				} else if (prior_error || startup == nullptr) {
					exit_code_ = 6;
				} else {
					exit_code_ = exit_code_for_phase(startup->phase());
				}
				return false;
			}
			if (stop_->stop_requested()) {
				break;	// e.g. Ctrl+C during a PTP wait
			}
		}

		initialized_ = true;
		return true;
	}

	void CaptureRunner::run_until_stop(const std::function<bool()> &external_stop) {
		if (!initialized_ || validate_only_) {
			return;
		}

		// Main loop: stats, limits, PTP offset reports.
		// max_duration_s measures CAPTURE time: the clock starts after bring-up
		// (discovery/PTP convergence can eat tens of seconds and must not count
		// against the configured duration).
		capture_start_mono_ = now_monotonic_ns();
		const double stats_interval_s = cfg_.stats.enabled ? cfg_.stats.interval_s : 0.0;
		const double offset_interval_s = cfg_.ptp.offset_report_interval_s;
		uint64_t last_stats_mono = capture_start_mono_;
		uint64_t last_offset_mono = capture_start_mono_;
		while (!stop_->stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			if (external_stop && external_stop()) {
				stop_->request_stop(StopReason::External);
				break;
			}
			const uint64_t now_mono = now_monotonic_ns();
			const uint64_t uptime_s = (now_mono - capture_start_mono_) / 1000000000ull;

			if (cfg_.acquisition.max_duration_s > 0 &&
				static_cast<double>(now_mono - capture_start_mono_) >= cfg_.acquisition.max_duration_s * 1e9) {
				LOG_INFO("max_duration_s (", cfg_.acquisition.max_duration_s, ") reached");
				stop_->request_stop(StopReason::LimitReached);
				break;
			}
			if (stats_interval_s > 0 && static_cast<double>(now_mono - last_stats_mono) >= stats_interval_s * 1e9) {
				const double actual = static_cast<double>(now_mono - last_stats_mono) / 1e9;
				last_stats_mono = now_mono;
				const uint64_t free_bytes = free_disk_bytes(session_dir_);
				for (auto &s: sessions_) {
					s->poll_stream_stats();
					LOG_INFO(s->reporter().periodic_line(actual, uptime_s, free_bytes));
				}
			}
			if (offset_interval_s > 0 && static_cast<double>(now_mono - last_offset_mono) >= offset_interval_s * 1e9) {
				last_offset_mono = now_mono;
				for (auto &s: sessions_) {
					s->refresh_ptp_offset();
				}
			}
		}
	}

	int CaptureRunner::shutdown() {
		if (shutdown_done_) {
			return exit_code_;
		}
		shutdown_done_ = true;
		if (!initialized_) {
			// init() already tore down, finalized and set the exit code.
			return exit_code_;
		}

		if (validate_only_) {
			if (stop_->stop_requested() && stop_->reason() != StopReason::None) {
				for (auto &s: sessions_) {
					s->stop_and_join();
				}
				finalize("failed", "interrupted during validation", true);
				events_.close();
				exit_code_ = stop_->reason() == StopReason::Error ? 6 : 130;
				return exit_code_;
			}
			for (auto &s: sessions_) {
				s->stop_and_join();
			}
			finalize("validated", "", true);
			events_.close();
			LOG_INFO("validate-only complete: configuration, PTP and preflight verified");
			exit_code_ = preflight_had_error_ ? 5 : 0;
			return exit_code_;
		}

		// Ordered shutdown + final accounting.
		LOG_INFO("stopping (reason: ", stop_reason_name(stop_->reason()), ")");
		events_.log("session_stop", nlohmann::ordered_json{ { "reason", stop_reason_name(stop_->reason()) } });
		for (auto &s: sessions_) {
			s->stop_and_join();
		}
		const uint64_t uptime_s = capture_start_mono_ != 0 ? (now_monotonic_ns() - capture_start_mono_) / 1000000000ull : 0;
		bool all_clean = true;
		for (auto &s: sessions_) {
			LOG_INFO(s->reporter().final_summary(uptime_s));
			all_clean = all_clean && s->clean();
		}
		finalize(stop_->reason() == StopReason::Error ? "failed" : "completed", "", true);
		events_.close();

		if (stop_->reason() == StopReason::Error) {
			exit_code_ = 6;
		} else {
			exit_code_ = all_clean ? 0 : 1;
		}
		return exit_code_;
	}

	void CaptureRunner::finalize(const std::string &status, const std::string &detail, bool with_cameras) {
		doc_["session"]["status"] = status;
		if (!detail.empty()) {
			doc_["session"]["detail"] = detail;
		}
		const uint64_t end_rt = now_realtime_ns();
		doc_["session"]["ended"] = iso8601_utc(end_rt);
		doc_["session"]["duration_s"] = (now_monotonic_ns() - start_mono_) / 1e9;
		doc_["session"]["stop_reason"] = stop_reason_name(stop_->reason());
		if (with_cameras) {
			nlohmann::ordered_json cams = nlohmann::ordered_json::array();
			for (const auto &s: sessions_) {
				cams.push_back(s->summary_json());
			}
			doc_["cameras"] = std::move(cams);
		}
		try {
			write_json_atomic(session_json_path_, doc_);
		} catch (const std::exception &e) {
			LOG_ERROR("cannot write session.json: ", e.what());
		}
	}

}  // namespace jai
