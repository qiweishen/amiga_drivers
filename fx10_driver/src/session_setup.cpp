#include "session_internal.h"

#include "pixel_format.h"
#include "wavelengths.h"
#include "time_util.h"

namespace fx10 {
using namespace session_detail;

Session::BringUp Session::BringUpSession() const {
    auto &cfg = impl_->config;
    impl_->session = std::make_unique<Impl::Connection>(cfg.network);
    auto &s = *impl_->session;

    const auto abandon = [this, &s](int exit_code, const std::string &error) {
        impl_->last_exit_code = exit_code;
        impl_->last_error = error;
        impl_->final_statistics["status"] = "startup_failed";
        impl_->final_statistics["exit_code"] = exit_code;
        impl_->final_statistics["error"] = error;
        impl_->final_statistics["counters"] = fx10::FinalCountersJson(s.counters);
        s.receiver.Disconnect();
        impl_->session.reset();
        return BringUp::kFailed;
    };
    // The eBUS calls below block without a cancellation API
    const auto stopped = [this, &s]() {
        if (!StopRequested()) {
            return false;
        }
        s.receiver.Disconnect();
        impl_->session.reset();
        return true;
    };

    try {
        // Serial port first
        if (cfg.sensor_trigger.enabled) {
            if (cfg.acquisition.trigger.mode == fx10::TriggerMode::kFreerun) {
                g_log.Warn("sensor_trigger is enabled but acquisition.trigger.mode is freerun — the camera "
                    "ignores the trigger pulses; the timing log still records exposure strobes");
            }
            g_log.Info("SensorSync observations and camera BlockIDs have independent counters; "
                       "use the line index and a verified anchor, never infer association from padding");
            if (!impl_->sync) {
                throw common::SensorSyncError("sensor_trigger.enabled but the host provides no SensorSync session "
                                              "(common::Config::sensor_sync is empty)");
            }
            // One board for the rig: registering opens it (STOP + idle check) before
            // any camera is armed; the pulses start once every participant armed
            const double pulse_hz = cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal
                                        ? cfg.acquisition.frame_rate_hz
                                        : 0.0;
            impl_->sync->Register(kSensorSyncOwner, cfg.sensor_trigger.trigger_channel, pulse_hz);
            impl_->sync_participant = true;
            // The ENVI session directory is <output_dir>/fx10_<UTC>/, two levels below raw/
            impl_->observations_file = std::filesystem::relative(impl_->sync->LogPath(),
                                                                 cfg.recording.output_dir / "session").generic_string();
        } else {
            // SensorSync is the FX10's only path to GPS time (host time is never a
            // time source on this platform)
            g_log.Warn("sensor_trigger.enabled=false: lines carry only the camera's raw GVSP tick; no GPS/PPS "
                       "time association is possible for this run");
        }
        if (stopped()) {
            return BringUp::kStopped;
        }
        s.receiver.Connect(cfg.device);
        if (stopped()) {
            return BringUp::kStopped;
        }
        // Factory baseline before the stream open: the load resets GevSCPSPacketSize
        if (!fx10::PrepareFactoryDefaults(s.receiver, [this] { return StopRequested(); })) {
            s.receiver.Disconnect();
            impl_->session.reset();
            return BringUp::kStopped;
        }
        s.control = std::make_unique<fx10::CameraControl>(*s.receiver.Device());
        if (stopped()) {
            return BringUp::kStopped;
        }
        s.receiver.OpenStream();

        s.control->ApplyAcquisitionConfig(cfg.acquisition, cfg.features.raw);
        s.geometry = s.control->ReadGeometry();
        if (stopped()) {
            return BringUp::kStopped;
        }

        const int expected_bands = cfg.acquisition.ExpectedBands();
        if (s.geometry.height != expected_bands) {
            g_log.Warn(
                "Camera reports {} image rows, config implies {} spectral bands; calibrated coverage is unverified",
                s.geometry.height, expected_bands);
        }

        const auto try_get_string = [&s](const char *node) {
            try {
                return s.control->GetString(node);
            } catch (const fx10::ControlError &) {
                return std::string("?");
            }
        };

        // Storage layout after the receiver's unpack step
        const auto *pf = fx10::GetPixelFormatInfo(cfg.acquisition.pixel_format);
        s.bytes_per_pixel = pf != nullptr ? pf->storage_bpp : 2;
        s.recorder_init.samples = static_cast<std::uint32_t>(s.geometry.width);
        s.recorder_init.bands = static_cast<std::uint32_t>(s.geometry.height);
        s.recorder_init.bytes_per_pixel = s.bytes_per_pixel;
        s.recorder_init.pixel_format = s.geometry.pixel_format;
        s.recorder_init.expected_frame_rate_hz = cfg.acquisition.frame_rate_hz;
        s.recorder_init.data_type = s.bytes_per_pixel == 1 ? fx10::EnviDataType::kUint8 : fx10::EnviDataType::kUint16;
        s.recorder_init.wavelengths = fx10::ResolveForGeometry(
            cfg.recording.wavelengths, try_get_string("DeviceSerialNumber"), s.geometry.width, s.geometry.height,
            s.geometry.offset_x, s.geometry.offset_y, cfg.acquisition.status_line);
        // What the CAMERA ended up with, not what the config asked for: both
        // nodes have their own increments and bounds, so the requested value is
        // not necessarily the one every line in this file was taken with.
        const auto read_back = [&s](const char *node, double fallback, const char *unit) {
            const auto v = s.control->TryGetFloat(node);
            return fmt::format("{:.4g} {}{}", v.value_or(fallback), unit, v ? "" : " (requested; not read back)");
        };
        s.recorder_init.description =
                "vendor: " + try_get_string("DeviceVendorName") +
                "\nmodel: " + try_get_string("DeviceModelName") +
                "\nserial: " + try_get_string("DeviceSerialNumber") +
                "\npixel format: " + s.geometry.pixel_format +
                "\nspatial binning (final readback verified): " + std::to_string(cfg.acquisition.spatial_binning) +
                "\nspectral binning (final readback verified): " + std::to_string(cfg.acquisition.spectral_binning) +
                "\nimage offsets (read back; -1 unknown): " + std::to_string(s.geometry.offset_x) + "," +
                    std::to_string(s.geometry.offset_y) +
                // The FX10 exposure node is microseconds; the config is ms.
                "\nexposure: " + read_back(fx10::node::kExposureTime, cfg.acquisition.exposure_ms * 1000.0, "us") +
                // Under an external trigger the camera's own rate node is
                // disabled, so its value would be a stale fiction: report the
                // commanded pulse rate instead.
                "\nframe rate: " + (cfg.acquisition.trigger.mode == fx10::TriggerMode::kFreerun
                                        ? read_back(fx10::node::kFrameRate, cfg.acquisition.frame_rate_hz, "Hz")
                                        : fmt::format("{:.4g} Hz (external trigger; commanded pulse rate)",
                                                      cfg.acquisition.frame_rate_hz)) +
                "\ntrigger: " + (cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal
                                     ? "external (" + cfg.acquisition.trigger.source_entry + ")"
                                     : "freerun") +
                // Whether every pixel in this file was resampled by the camera's
                // wavelength/smile/keystone correction (manual p.32-33) — and
                // therefore which calibration pack applies to it (p.44).
                "\nimage enhancement (AIE): UNKNOWN (node not read back; configuration requests on)" +
                "\nspectral calibration: " + (cfg.recording.wavelengths.source == fx10::WavelengthSource::kNone
                    ? "UNCALIBRATED DN; image rows are not validated spectral bands"
                    : "operator-supplied profile; geometry/serial checked, physical calibration not independently verified") +
                // Not written by this driver: switching it would invalidate the
                // factory BlackLevelOffset and image corrections (manual p.22).
                "\nreadout mode: camera factory setting (interleave untouched)" +
                "\nstatus line (final readback verified): " + (cfg.acquisition.status_line ? "on (REPLACES the last image row)" : "off") +
                (cfg.acquisition.mroi.enabled
                     ? "\nmroi: " + cfg.acquisition.mroi.multiband_string
                     : "") +
                "\nline identity: segment_NNNN.lines.csv; trigger association requires a verified anchor" +
                (impl_->sync_participant
                     ? "\ntiming observations: " + impl_->observations_file +
                       " (SensorSync-Logger; one session per rig, channel " +
                       std::to_string(cfg.sensor_trigger.trigger_channel) + ")"
                     : "");
    } catch (const fx10::ConfigError &e) {
        // A configuration value only the camera could reject: the wavelength
        // axis against the camera's band count, or an MROI role group the
        // camera does not expose.
        g_log.Error("{}", e.what());
        return abandon(1, e.what());
    } catch (const std::exception &e) {
        // TransportError / ControlError
        g_log.Error("{}", e.what());
        return abandon(2, e.what());
    }

    return BringUp::kOk;
}


bool Session::StartStreaming() {
    auto &cfg = impl_->config;
    auto &s = *impl_->session;
    impl_->stop_reason = "completed";
    impl_->last_exit_code = 0;
    impl_->last_error.clear();

    s.recorder = std::make_unique<fx10::EnviRecorder>(cfg.recording, s.counters);
    try {
        s.recorder->Start(s.recorder_init);
        s.telemetry.Open(s.recorder->SessionDir() / "telemetry.jsonl");
    } catch (const std::exception &e) {
        g_log.Error("{}", e.what());
        impl_->last_error = e.what();
        impl_->last_exit_code = 20;
        impl_->stop_reason = "recorder-start-failed";
        TeardownSession();
        return false;
    }

    fx10::ExpectedGeometry expected;
    expected.width = static_cast<std::uint32_t>(s.geometry.width);
    expected.height = static_cast<std::uint32_t>(s.geometry.height);
    expected.bytes_per_pixel = s.bytes_per_pixel;
    expected.payload_size = s.geometry.payload_size;
    expected.pixel_format = cfg.acquisition.pixel_format;
    expected.status_line = cfg.acquisition.status_line;

    try {
        // Factory preparation and ALL configuration writes are complete. Normal
        // scene acquisition requires a new open pulse, regardless of the stored
        // pulse value. Write acknowledgement does not report mechanical position.
        s.control->OpenShutter([this] { return StopRequested(); });
        s.receiver.Start(*s.recorder, expected, cfg.acquisition.frame_rate_hz, [&] {
            auto device = s.control->CollectDeviceMetadata();
            const auto &counter_source = device["final_readback"][fx10::node::kMissedTriggerSource];
            s.counter_binding_verified = counter_source["status"] == "ok" && counter_source["value"] == "MissedTrigger";
            device["created_realtime_ns"] = common::TimeUtil::RealtimeNowNs();
            device["snapshot_phase"] = "after buffer allocation, before StreamEnable/AcquisitionStart";
            device["factory_preparation"] = {{"commands", {"CameraHeadFactoryReset", "UserSetLoad"}},
                {"completed", true}, {"semantics", "commands acknowledged and control readiness checked; undocumented factory values not audited"}};
            device["runtime"] = s.receiver.RuntimeMetadata();
            device["network_requested"] = {{"packet_size", cfg.network.packet_size},
                {"socket_rx_buffer_mb", cfg.network.socket_rx_buffer_mb}, {"buffer_count", cfg.network.buffer_count},
                {"stall_budget_s", cfg.network.stall_budget_s}, {"max_buffer_memory_mb", cfg.network.max_buffer_memory_mb},
                {"retrieve_timeout_ms", cfg.network.retrieve_timeout_ms}};
            device["recording_policy"] = {{"rotation_max_lines", cfg.recording.rotation.max_lines},
                {"rotation_max_mb", cfg.recording.rotation.max_mb}, {"flush_interval_mb", cfg.recording.flush_interval_mb},
                {"on_gap", cfg.recording.on_gap == fx10::GapPolicy::kPadZero ? "pad_zero" : "record"},
                {"producer_metadata", "../../drivers.json"}};
            device["config_source"] = config_path_;
            device["connection_path"] = cfg.device.mac.empty() ? "direct_ip" : "mac_discovery";
            device["recording_contract"] = fx10::RecordingContract(s.geometry.pixel_format);
            device["image_enhancement"] = {{"requested", cfg.acquisition.image_enhancement},
                {"readback", nullptr}, {"status", "unknown; node not verified"}};
            device["mroi_requested"] = cfg.acquisition.mroi.multiband_string;
            device["mroi_readback_semantics"] = "per-region values in ordered applied entries; final selector positions are not a region table";
            const auto &cal = cfg.recording.wavelengths.calibration;
            device["calibration"] = {{"reference", cal.reference}, {"device_serial", cal.device_serial},
                {"samples", cal.samples}, {"bands", cal.bands}, {"offset_x", cal.offset_x}, {"offset_y", cal.offset_y},
                {"wavelengths_nm", s.recorder_init.wavelengths.nm}, {"fwhm_nm", s.recorder_init.wavelengths.fwhm},
                {"source", s.recorder_init.wavelengths.source_tag},
                {"geometry_profile_verified", cfg.recording.wavelengths.source != fx10::WavelengthSource::kNone},
                {"physical_calibration_verified", false}};
            device["timing"] = {{"mode", cfg.acquisition.trigger.mode == fx10::TriggerMode::kExternal ? "external" : "freerun"},
                {"expected_line_rate_hz", cfg.acquisition.frame_rate_hz},
                {"rate_semantics", "configured expectation; external pulse rate is not camera-measured line rate"},
                {"sensor_trigger_enabled", cfg.sensor_trigger.enabled}, {"trigger_channel", cfg.sensor_trigger.trigger_channel},
                {"observations_file", impl_->sync_participant ? nlohmann::json(impl_->observations_file) : nlohmann::json(nullptr)},
                {"observations_scope", "one SensorSync session per rig: every triggered camera's events share this log, keyed by channel"},
                {"association_verified", false}, {"association_anchor", nullptr}};
            try { fx10::PublishMetadata(s.recorder->SessionDir() / "device.json", device); }
            catch (...) { s.metadata_failed = true; throw; }
            // No extra reads or disk barrier between camera arming and SensorSync logging.
            if (!s.RecordTelemetry("start", true)) throw fx10::MetadataError("startup telemetry failed");
            if (StopRequested()) throw fx10::ControlError("[eBUS] acquisition start interrupted");
        });
        s.acquisition_started = true;
    } catch (const std::exception &e) {
        g_log.Error("{}", e.what());
        impl_->last_error = e.what();
        impl_->last_exit_code = s.metadata_failed ? 20 : 3;
        impl_->stop_reason = "start-failed";
        TeardownSession();
        return false;
    }

    // The camera is armed: tell the shared SensorSync session. Pulses start when
    // every registered camera has armed (the Go-X arms during its Init, so this
    // is normally the arm that starts them). This ordering alone does not prove
    // a one-to-one frame association (freerun = trigger channel off).
    if (impl_->sync_participant) {
        try {
            if (impl_->sync->Arm(kSensorSyncOwner)) {
                g_log.Info("SensorSync session running; timing log {}", impl_->sync->LogPath().string());
            } else {
                g_log.Info("SensorSync: camera armed, pulses start once the other participant(s) arm");
            }
        } catch (const common::SensorSyncError &e) {
            g_log.Error("{}", e.what());
            impl_->last_error = e.what();
            impl_->last_exit_code = 20;
            impl_->stop_reason = "trigger-log-start-failed";
            TeardownSession();
            return false;
        }
    }

    return true;
}


} // namespace fx10
