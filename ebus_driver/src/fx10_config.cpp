#include "ebus_driver/camera_driver.h"

#include "../../fx10_driver/include/app_config.h"
#include "../../fx10_driver/include/ebus/camera_control.h"
#include "../../fx10_driver/include/wavelengths.h"
#include "logger.h"
#include "time_util.h"

#include <PvDeviceGEV.h>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>

namespace ebus_driver {
    namespace {
        common::DriverLog log{"FX10"};

        struct Fx10State {
            fx10::AppConfig config;
            std::filesystem::path source;
            std::unique_ptr<fx10::CameraControl> control;
            fx10::CameraControl::Geometry geometry;
            fx10::Wavelengths wavelengths;
            bool dark = false, counter_bound = false, counter_regressed = false, thermal_warning = false;
            std::optional<std::int64_t> baseline, previous;
        };

        void CheckStop(const StopPredicate &stop) {
            if (stop && stop()) {
                throw std::runtime_error("FX10 preparation interrupted");
            }
        }
    }


    CameraDefinition MakeFx10Camera(const fx10::AppConfig &config,
                                    const std::filesystem::path &config_source,
                                    const std::filesystem::path &output, bool dark_reference) {
        auto state = std::make_shared<Fx10State>();
        state->config = config;
        state->source = config_source;
        state->dark = dark_reference;
        CameraDefinition definition;
        auto &options = definition.options;
        options.id = "fx10";
        options.kind = "fx10";
        options.module = "FX10";
        options.mac = config.device.mac;
        options.ip = config.device.ip;
        options.recording.directory = output;
        if (config.recording.rotation.max_mb > (std::numeric_limits<std::uint64_t>::max() >> 20)) {
            throw std::runtime_error("FX10 rotation.max_mb overflows the RAW segment capacity");
        }
        options.recording.segment_bytes = config.recording.rotation.max_mb != 0
                                              ? config.recording.rotation.max_mb << 20
                                              : std::numeric_limits<std::uint64_t>::max();
        options.recording.segment_records = config.recording.rotation.max_lines;
        options.recording.flush_bytes = static_cast<std::uint64_t>(config.recording.flush_interval_mb) << 20;
        options.packet_size = static_cast<std::uint32_t>(config.network.packet_size);
        options.socket_rx_buffer_mb = static_cast<std::uint32_t>(config.network.socket_rx_buffer_mb);
        options.sdk_buffers = config.network.buffer_count > 0
                                  ? static_cast<std::uint32_t>(config.network.buffer_count)
                                  : 16;
        options.max_buffer_memory_mb = static_cast<std::uint32_t>(config.network.max_buffer_memory_mb);
        options.retrieve_timeout_ms = static_cast<std::uint32_t>(config.network.retrieve_timeout_ms);
        options.stall_budget_s = config.network.stall_budget_s;
        options.expected_rate_hz = config.acquisition.frame_rate_hz;
        options.telemetry_interval_s = 60.0;
        options.statistics_interval_s = config.logging.stats_interval_s;
        options.max_duration_s = config.recording.max_duration_s;
        options.max_frames = config.recording.max_frames;
        options.sensor_sync = config.sensor_trigger.enabled;
        options.sensor_channel = config.sensor_trigger.trigger_channel;
        const bool external = config.acquisition.trigger.mode == fx10::TriggerMode::kExternal;
        options.pulse_hz = options.sensor_sync && external ? config.acquisition.frame_rate_hz : 0.0;
        options.expect_data = !external || options.sensor_sync;

        auto &hooks = definition.hooks;
        hooks.prepare = [state](PvDeviceGEV &device, const StopPredicate &stop) {
            if (!fx10::PrepareFactoryDefaults(device, stop)) {
                throw std::runtime_error("FX10 factory preparation interrupted");
            }
            state->control = std::make_unique<fx10::CameraControl>(device);
        };
        hooks.configure = [state](PvDeviceGEV &, const StopPredicate &stop) {
            CheckStop(stop);
            auto &control = *state->control;
            control.ApplyAcquisitionConfig(state->config.acquisition, state->config.features.raw);
            state->geometry = control.ReadGeometry();
            const auto &geometry = state->geometry;
            if (geometry.width <= 0 || geometry.height <= 0 || geometry.payload_size == 0) {
                throw std::runtime_error("FX10 camera returned unusable geometry");
            }
            const auto serial = state->config.recording.wavelengths.source == fx10::WavelengthSource::kNone
                                    ? std::string()
                                    : control.GetString("DeviceSerialNumber");
            state->wavelengths = fx10::ResolveForGeometry(state->config.recording.wavelengths, serial,
                                                          geometry.width, geometry.height, geometry.offset_x,
                                                          geometry.offset_y, state->config.acquisition.status_line);
            const auto source = control.ReadNodeMetadata(fx10::node::kMissedTriggerSource);
            state->counter_bound = source["status"] == "ok" && source["value"] == "MissedTrigger";
            CheckStop(stop);
        };
        hooks.before_start = [state](PvDeviceGEV &, const StopPredicate &stop) {
            if (state->dark) state->control->CloseShutter(stop);
            else state->control->OpenShutter(stop);
            state->baseline = state->control->TryGetInt(fx10::node::kMissedTriggerCount);
            state->previous = state->baseline;
            CheckStop(stop);
            return nlohmann::json{
                {"shutter_preparation", state->control->ShutterPreparation()},
                {
                    "missed_triggers", {
                        {"baseline", state->baseline ? nlohmann::json(*state->baseline) : nlohmann::json(nullptr)},
                        {"binding_verified", state->counter_bound}
                    }
                }
            };
        };
        hooks.metadata = [state](PvDeviceGEV &) {
            auto doc = state->control->CollectDeviceMetadata();
            // The common engine supplies the SDK format identities.
            doc["kind"] = "fx10";
            doc["id"] = "fx10";
            doc["config_source"] = state->source.string();
            const auto &geometry = state->geometry;
            const auto &cal = state->config.recording.wavelengths.calibration;
            const int full_scale = geometry.pixel_format.find("12") != std::string::npos
                                       ? 4095
                                       : geometry.pixel_format.find("10") != std::string::npos
                                             ? 1023
                                             : geometry.pixel_format == "Mono8"
                                                   ? 255
                                                   : 0;
            doc["profile"] = {
                {"samples", geometry.width}, {"bands", geometry.height},
                {"offset_x", geometry.offset_x}, {"offset_y", geometry.offset_y},
                {"pixel_format", geometry.pixel_format}, {"full_scale", full_scale},
                {"expected_frame_rate_hz", state->config.acquisition.frame_rate_hz},
                {"wavelengths_nm", state->wavelengths.nm}, {"fwhm_nm", state->wavelengths.fwhm},
                {"wavelength_source", state->wavelengths.source_tag},
                {"frame_axes", "SDK width=spatial samples; SDK height=spectral rows; one SDK image is one scan line"},
                {"status_line", state->config.acquisition.status_line},
                {
                    "image_enhancement",
                    {{"requested", state->config.acquisition.image_enhancement}, {"readback", nullptr}}
                },
                {
                    "calibration", {
                        {"reference", cal.reference}, {"device_serial", cal.device_serial},
                        {"samples", cal.samples}, {"bands", cal.bands}, {"offset_x", cal.offset_x},
                        {"offset_y", cal.offset_y}
                    }
                },
                {"shutter_request", state->dark ? "closed-dark-reference" : "open"}
            };
            doc["legacy_output_options"] = {
                {"on_gap", "no padding in RAW; gaps are metadata events"},
                {"rotation_max_lines_requested", state->config.recording.rotation.max_lines},
                {"rotation_max_lines_semantics", "RAW SDK records, including flagged/non-image buffers"},
                {"envi", "offline export only"}
            };
            doc["timing"] = {
                {
                    "mode",
                    state->config.acquisition.trigger.mode == fx10::TriggerMode::kExternal ? "external" : "freerun"
                },
                {"sensor_trigger_enabled", state->config.sensor_trigger.enabled},
                {"trigger_channel", state->config.sensor_trigger.trigger_channel},
                {"association_verified", false}, {"device_timestamp_unit", "unverified"},
                {"device_timestamp_epoch", "unverified"}
            };
            doc["factory_preparation"] = {
                {"commands", {"CameraHeadFactoryReset", "UserSetLoad"}},
                {"completed", true},
                {"semantics", "command acknowledgement and control readiness; undocumented defaults not audited"}
            };
            return doc;
        };
        hooks.sample = [state](PvDeviceGEV &) {
            HealthSample health;
            if (!state->control) {
                health.values["status"] = "configuration_not_completed";
                return health;
            }
            const auto raw = state->control->TryGetInt(fx10::node::kMissedTriggerCount);
            const auto pcb = state->control->TryReadTemperature("ProcPCB");
            const auto fpga = state->control->TryReadTemperature("FPGA");
            if (raw && (*raw < 0 || (state->previous && *raw < *state->previous))) {
                state->counter_regressed = true;
            }
            if (raw) {
                state->previous = raw;
            }
            std::optional<std::int64_t> delta;
            if (state->counter_bound && raw && state->baseline && *state->baseline >= 0 && !state->counter_regressed) {
                delta = *raw - *state->baseline;
            }
            health.values = {
                {"host_realtime_ns", common::TimeUtil::RealtimeNowNs()},
                {"temp_proc_pcb_c", pcb ? nlohmann::json(*pcb) : nlohmann::json(nullptr)},
                {"temp_fpga_c", fpga ? nlohmann::json(*fpga) : nlohmann::json(nullptr)},
                {
                    "missed_triggers", {
                        {"raw", raw ? nlohmann::json(*raw) : nlohmann::json(nullptr)},
                        {"baseline", state->baseline ? nlohmann::json(*state->baseline) : nlohmann::json(nullptr)},
                        {"delta", delta ? nlohmann::json(*delta) : nlohmann::json(nullptr)},
                        {"binding_verified", state->counter_bound}, {"counter_regressed", state->counter_regressed}
                    }
                }
            };
            if (state->counter_bound && state->counter_regressed) {
                health.failure = "FX10 missed-trigger counter regressed";
            } else if (delta && *delta > 0) health.failure = "FX10 camera reported missed triggers";
            const bool hot = (pcb && *pcb >= 75.0) || (fpga && *fpga >= 85.0);
            const bool cooled = (!pcb || *pcb <= 70.0) && (!fpga || *fpga <= 80.0);
            if (!state->thermal_warning && hot) {
                state->thermal_warning = true;
                log.Warn("Camera approaching its documented thermal limit; temperatures retained in RAW events");
            } else if (cooled) {
                state->thermal_warning = false;
            }
            return health;
        };
        return definition;
    }


    CameraDefinition LoadFx10Camera(const std::filesystem::path &config, const std::filesystem::path &output) {
        return MakeFx10Camera(fx10::LoadAppConfig(config.string()), config, output);
    }
} // namespace ebus_driver
