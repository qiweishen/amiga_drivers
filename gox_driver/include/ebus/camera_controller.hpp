#pragma once

// Device control plane: owns the PvDeviceGEV, applies the typed GenICam
// configuration engine (ordered application + write-then-read-back
// verification per plan section 2), takes the reproducibility snapshots and
// wraps StreamEnable/AcquisitionStart/Stop. The link-disconnected event sink
// feeds straight into the process-wide StopController (v1: no reconnect).

#include <PvDevice.h>
#include <PvDeviceGEV.h>
#include <PvGenParameter.h>
#include <PvGenParameterArray.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/session_log.hpp"
#include "core/signal_stop.hpp"
#include "ebus/discovery.hpp"

namespace jai::ebus {

	// Outcome of one feature application, kept for session.json.
	struct AppliedFeature {
		std::string name;
		std::string requested;
		std::string readback;  // empty when verification was off / not applicable
		std::string status;	   // "ok" | "warned: ..." | "skipped: ..."
	};

	// Generic typed GenICam write. Dispatches on PvGenParameter::GetType()
	// (Integer/Float/Boolean/Enum/String/Command) and, when
	// apply.verify_readback is set, reads the value back and verifies it:
	// Integer within GetIncrement() rounding, Float within
	// float_verify_tolerance_rel (relative), everything else exact. Commands are
	// executed, never verified. On failure the effective on_error policy
	// (f.on_error, falling back to apply.on_error_default) decides: "fail"
	// throws SdkError, "warn" logs a warning, "skip" logs at debug level.
	// Returns true when the value was applied (and verified). `record`, when
	// non-null, receives the outcome regardless.
	bool apply_genicam_feature(PvGenParameterArray *params, const GenicamFeature &f, const ApplyConfig &apply,
							   const std::string &context, AppliedFeature *record = nullptr);

	// By-name read/execute helpers shared with ptp_manager and stream_receiver.
	// All return false (without logging) when the parameter is missing or the
	// access fails, so callers can silently skip optional features.
	bool feature_exists(PvGenParameterArray *params, const std::string &name);
	bool read_int_feature(PvGenParameterArray *params, const std::string &name, int64_t &out);
	bool read_float_feature(PvGenParameterArray *params, const std::string &name, double &out);
	bool read_enum_feature(PvGenParameterArray *params, const std::string &name, std::string &out);
	bool read_feature_as_string(PvGenParameterArray *params, const std::string &name, std::string &out);
	bool execute_command_feature(PvGenParameterArray *params, const std::string &name);

	class CameraController : protected PvDeviceEventSink {
	public:
		CameraController(std::string camera_id, StopController *stop, EventLog *events);
		~CameraController();

		CameraController(const CameraController &) = delete;
		CameraController &operator=(const CameraController &) = delete;

		// Connects with PvAccessControl (not Exclusive: keeps eBUS Player
		// read-only debugging possible), sets communication parameters
		// (AnswerTimeout/CommandRetries) and registers the link-disconnected
		// sink. `info` must still be owned by a live Discovery instance;
		// `identity` is the plain-data snapshot kept for the session metadata.
		// Warns loudly when the vendor name does not start with "JAI".
		void connect(const PvDeviceInfoGEV *info, const DiscoveredDevice &identity);

		// Unregisters the sink and disconnects. Idempotent.
		void disconnect();

		bool connected() const;
		bool link_lost() const { return link_lost_.load(std::memory_order_relaxed); }

		// Ordered configuration per plan: autos off -> binning (hoisted from
		// genicam_features) -> offsets zeroed -> Width -> Height -> OffsetX/Y ->
		// PixelFormat -> exposure/gain/frame_rate/trigger convenience mappings ->
		// remaining genicam_features in listed order -> GevGVSPExtendedIDMode
		// forced On when Off -> GevTimestampTickFrequency read. Throws SdkError
		// when a feature with policy "fail" cannot be applied.
		void apply_config(const CameraConfig &cfg);

		// Reproducibility snapshot: full parameter-array walk to a text file
		// (category/name = value, unreadable entries kept with a placeholder)
		// plus PvConfigurationWriter::Store to a .pvcfg. Best-effort: failures
		// are logged, never thrown.
		void write_feature_snapshot(const std::string &text_path, const std::string &pvcfg_path);

		// TLParamsLocked management + acquisition commands (PvGenCommand).
		void stream_enable();
		void stream_disable(bool ignore_errors);
		void acquisition_start();
		void acquisition_stop(bool ignore_errors);

		uint32_t payload_size();

		PvGenParameterArray *params();
		PvDeviceGEV *device() { return device_.get(); }
		const DiscoveredDevice &identity() const { return identity_; }

		// GevTimestampTickFrequency read during apply_config(); 0 when absent.
		int64_t timestamp_tick_frequency() const { return tick_frequency_; }

		// Applied-feature list (ordered) for session.json.
		nlohmann::ordered_json applied_json() const;

	protected:
		// PvDeviceEventSink
		void OnLinkDisconnected(PvDevice *device) override;

	private:
		// Applies name=value with an explicit policy. When required is false a
		// missing parameter is a silent (debug-level) no-op returning false.
		bool try_apply(const std::string &name, const std::string &value, bool value_is_string, const ApplyConfig &apply,
					   const std::string &policy, bool required);

		std::string camera_id_;
		StopController *stop_;
		EventLog *events_;

		std::unique_ptr<PvDeviceGEV> device_;
		DiscoveredDevice identity_;
		std::vector<AppliedFeature> applied_;
		std::atomic<bool> link_lost_{ false };
		int64_t tick_frequency_ = 0;
	};

}  // namespace jai::ebus
