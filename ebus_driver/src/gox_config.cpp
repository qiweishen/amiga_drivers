#include "ebus_driver/camera_driver.h"

#include "../../gox_driver/include/app_config.h"
#include "../../gox_driver/include/buffering.h"
#include "../../gox_driver/include/device_json.h"
#include "../../gox_driver/include/ebus/camera_controller.h"
#include "../../gox_driver/include/ebus/ptp_manager.h"
#include "logger.h"

#include <PvStreamGEV.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>


namespace ebus_driver {
    namespace {
        common::DriverLog log{"GoX"};


        struct GoxState {
            gox::CameraConfig camera;
            gox::OutputConfig output;
            gox::PtpConfig ptp_config;
            bool sensor_sync = false;
            std::filesystem::path source;
            StopPredicate stop;
            std::unique_ptr<gox::ebus::CameraController> control;
            std::unique_ptr<gox::ebus::PtpManager> ptp;
            gox::FactoryLoadResult factory;
            std::vector<gox::AppliedFeature> applied;
            bool counter_bound = false, counter_reset = false, counter_regressed = false;
            bool counter_started = false, counter_binding_lost = false, counter_status_unavailable = false;
            bool counter_negative = false, counter_input_unverified = false, counter_value_unavailable = false;
            bool counter_overflow = false, thermal_warning = false;
            std::optional<std::int64_t> baseline, previous;
        };


        void CheckStop(const StopPredicate &stop) {
            if (stop && stop()) {
                throw std::runtime_error("GOX preparation interrupted");
            }
        }


        nlohmann::json Readback(gox::ebus::CameraController &control, const char *name) {
            std::string value;
            if (!gox::ebus::ReadFeatureAsString(control.Params(), name, value)) {
                return nullptr;
            }
            return value;
        }
    }


    std::vector<CameraDefinition> MakeGoxCameras(const gox::AppConfig &config,
                                                 const std::filesystem::path &config_source,
                                                 const std::filesystem::path &output) {
        std::vector<CameraDefinition> definitions;
        for (const auto &camera: config.cameras) {
            if (!camera.enabled) {
                continue;
            }
            if (!camera.acquisition.pixel_format ||
                *camera.acquisition.pixel_format != gox::kNativePixelFormat) {
                throw std::invalid_argument("GOX acquisition requires PixelFormat=BayerRG12Packed");
            }
            auto state = std::make_shared<GoxState>();
            state->camera = camera;
            state->output = config.output;
            state->ptp_config = config.ptp;
            state->sensor_sync = config.sensor_trigger.enabled;
            state->source = config_source;
            CameraDefinition definition;
            auto &options = definition.options;
            options.id = camera.id;
            options.kind = "gox";
            options.module = "GoX";
            options.mac = camera.device.mac;
            options.ip = camera.device.ip;
            options.local_ip = camera.network.local_ip;
            options.recording.directory = output / camera.id;
            options.recording.segment_bytes = static_cast<std::uint64_t>(config.output.segment_size_mb) << 20;
            options.recording.flush_bytes = static_cast<std::uint64_t>(config.output.flush_interval_mb) << 20;
            options.packet_size = camera.network.packet_size;
            options.socket_rx_buffer_mb = camera.network.socket_rx_buffer_mb;
            options.expected_rate_hz = camera.acquisition.frame_rate_hz.value_or(1.0);
            options.sdk_buffers = camera.network.buffer_count != 0
                                      ? camera.network.buffer_count
                                      : gox::AutoBufferCount(camera.acquisition.frame_rate_hz.value_or(0.0), 0);
            options.queue_frames = config.output.queue_max_frames != 0
                                       ? config.output.queue_max_frames
                                       : gox::AutoQueueFrames(camera.acquisition.frame_rate_hz.value_or(0.0));
            options.max_duration_s = config.output.max_duration_s;
            options.max_frames = config.output.max_frames;
            options.sensor_sync = config.sensor_trigger.enabled;
            options.sensor_channel = camera.acquisition.trigger.sensor_channel;
            const bool external = camera.acquisition.trigger.mode == gox::TriggerMode::kExternal;
            options.pulse_hz = options.sensor_sync && external ? camera.acquisition.frame_rate_hz.value_or(0.0) : 0.0;
            options.expect_data = !external || options.sensor_sync;
            options.extended_block_ids = true; // configure below requires the final On readback
            options.telemetry_interval_s = 5.0;
            options.statistics_interval_s = config.stats_interval_s;

            if (!state->ptp_config.enabled && camera.acquisition.trigger.mode != gox::TriggerMode::kExternal) {
                log.Warn(
                        "[{}] Both PTP and External Trigger are disabled, no absolute timestamp for images",
                        state->camera.id);
            }

            auto &hooks = definition.hooks;
            hooks.prepare = [state](PvDeviceGEV &device, const StopPredicate &stop) {
                state->stop = stop;
                CheckStop(stop);
                log.Info(
                    "[{}] ebus-raw output: no record alignment, queue exhaustion fails the session, safe error buffers are retained",
                    state->camera.id);
                if (state->output.record_align != gox::OutputConfig{}.record_align ||
                    state->output.queue_on_full != gox::OutputConfig{}.queue_on_full ||
                    state->output.on_buffer_error != gox::OutputConfig{}.on_buffer_error)
                    log.Warn(
                        "[{}] Legacy output policy overrides are superseded by the unified RAW policy; requested/effective values are retained in device.json",
                        state->camera.id);
                state->control = std::make_unique<gox::ebus::CameraController>(state->camera.id, device);
                state->factory = state->control->LoadFactoryDefaults(stop);

                // Trigger-only and freerun capture do not need a PTP manager.
                if (state->ptp_config.enabled) {
                    state->ptp = std::make_unique<gox::ebus::PtpManager>(
                        state->camera.id, device.GetParameters(), state->ptp_config);
                    if (!state->ptp->Enable() || !state->ptp->WaitForSync(stop)) {
                        throw std::runtime_error("GOX PTP startup requirement was not met");
                    }
                }
                CheckStop(stop);
            };

            hooks.configure = [state](PvDeviceGEV &, const StopPredicate &stop) {
                CheckStop(stop);
                state->applied = state->control->ApplyConfig(state->camera);
                std::string extended;
                if (!gox::ebus::ReadEnumFeature(state->control->Params(), "GevGVSPExtendedIDMode", extended) || extended
                    != "On")
                    throw std::runtime_error(
                        "GOX requires verified GevGVSPExtendedIDMode=On; remove a conflicting raw override");
                // The raw escape hatch can change these nodes or reload a user set.
                // Verify the final device state before the engine sizes buffers or starts capture.
                std::string pixel_format;
                std::int64_t digitization_bits = 0;
                if (!gox::ebus::ReadEnumFeature(state->control->Params(), "PixelFormat", pixel_format) ||
                    pixel_format != gox::kNativePixelFormat ||
                    !gox::ebus::ReadEnumIntFeature(state->control->Params(), "SensorDigitizationBits",
                                                 digitization_bits) || digitization_bits != 12) {
                    throw std::runtime_error(
                        "GOX requires verified PixelFormat=BayerRG12Packed and SensorDigitizationBits=12; "
                        "remove a conflicting raw override");
                }
                state->counter_bound = state->control->TriggerCounterBound();
                CheckStop(stop);
            };

            hooks.configure_stream = [state](PvStreamGEV &stream) {
                for (const auto &raw: state->camera.network.receiver_tuning) {
                    gox::FeatureWrite write;
                    write.name = raw.name;
                    write.value = raw.value;
                    write.value_is_string = raw.value_is_string;
                    write.strict = false;
                    gox::ebus::apply_genicam_feature(stream.GetParameters(), write,
                                                     "[" + state->camera.id + "] stream");
                }
            };

            hooks.before_start = [state](PvDeviceGEV &, const StopPredicate &stop) {
                CheckStop(stop);
                state->counter_reset = state->counter_bound && state->control->ResetTriggerCounter();
                const auto baseline = state->control->SampleTelemetry(state->counter_bound);
                state->baseline = baseline.trig;
                state->previous = baseline.trig;
                state->counter_started = true;
                state->counter_binding_lost = state->counter_bound && !baseline.trig_binding_verified;
                state->counter_status_unavailable = !baseline.trig_status_read;
                state->counter_input_unverified = !baseline.trig_input_verified;
                state->counter_value_unavailable = !baseline.trig;
                state->counter_negative = baseline.trig && *baseline.trig < 0;
                state->counter_overflow = baseline.trig_overflow;
                if (state->counter_bound && !state->counter_reset)
                    log.Warn("[{}] CounterReset not confirmed; trigger baseline is recorded explicitly",
                             state->camera.id);
                return nlohmann::json{
                    {
                        "trigger_counter", {
                            {"reset_attempted", state->counter_bound},
                            {"reset_acknowledged", state->counter_reset},
                            {"baseline_sampled", state->counter_started},
                            {"baseline", state->baseline ? nlohmann::json(*state->baseline) : nlohmann::json(nullptr)}
                        }
                    }
                };
            };

            hooks.metadata = [state](PvDeviceGEV &device) {
                gox::RuntimeShape runtime;
                runtime.payload_size = device.GetPayloadSize();
                const auto ptp = state->ptp ? state->ptp->Summary() : gox::PtpSummary{};
                auto report = state->control->CollectDeviceReport(
                    state->camera, state->applied, ptp, state->factory, runtime);
                report.timing.mode = state->camera.acquisition.trigger.mode == gox::TriggerMode::kExternal
                                         ? "external"
                                         : "freerun";
                report.timing.trigger_source = state->camera.acquisition.trigger.source_entry;
                report.timing.sensor_trigger_enabled = state->sensor_sync;
                report.timing.sensor_channel = state->sensor_sync
                                                   ? state->camera.acquisition.trigger.sensor_channel
                                                   : -1;
                report.timing.exposure_active_output = state->camera.acquisition.trigger.exposure_active_output;
                if (state->sensor_sync && report.timing.mode == "external") {
                    report.timing.expected_pulse_rate_hz = state->camera.acquisition.frame_rate_hz;
                }
                nlohmann::json doc = gox::BuildDeviceJson(report);
                // The engine supplies SDK format identities, allocated runtime and RAW contract.
                doc.erase("runtime");
                doc.erase("recording_contract");
                doc["kind"] = "gox";
                doc["id"] = state->camera.id;
                doc["config_source"] = state->source.string();
                doc["legacy_output_options"] = {
                    {
                        "record_align", {
                            {"requested", state->output.record_align}, {"effective", 1},
                            {"semantics", "RAW records are contiguous; no alignment padding"}
                        }
                    },
                    {
                        "queue_on_full", {
                            {
                                "requested",
                                state->output.queue_on_full == gox::QueueOnFull::kBlock ? "block" : "drop_newest"
                            },
                            {"effective", "fail_session_and_audit_rejected_buffer"}
                        }
                    },
                    {
                        "on_buffer_error", {
                            {
                                "requested",
                                state->output.on_buffer_error == gox::OnBufferError::kDrop ? "drop" : "record_flagged"
                            },
                            {"effective", "retain_all_safe_payload_bytes_and_fail_session_on_operation_error"}
                        }
                    }
                };
                doc["selector_restore_failed"] = report.runtime.selector_restore_failed;
                doc["profile"] = {
                    {"pixel_format", Readback(*state->control, "PixelFormat")},
                    {"width", Readback(*state->control, "Width")}, {"height", Readback(*state->control, "Height")},
                    {"frame_axes", "area image; no host debayer or pixel conversion"},
                    {
                        "expected_frame_rate_hz",
                        state->camera.acquisition.frame_rate_hz
                            ? nlohmann::json(*state->camera.acquisition.frame_rate_hz)
                            : nlohmann::json(nullptr)
                    }
                };
                doc["trigger_counter"] = {{"bound_at_configure", state->counter_bound}};
                if (report.runtime.selector_restore_failed) {
                    throw std::runtime_error("GOX metadata audit could not restore feature selectors");
                }
                return doc;
            };

            hooks.sample = [state](PvDeviceGEV &) {
                HealthSample health;
                if (!state->control || (state->ptp_config.enabled && !state->ptp)) {
                    health.values["status"] = "configuration_not_completed";
                    return health;
                }
                if (state->ptp && !state->ptp->CheckHealth(state->stop)) {
                    health.failure = "GOX PTP synchronization lost";
                }
                const auto sample = state->control->SampleTelemetry(state->counter_bound);
                health.values = nlohmann::json::parse(gox::BuildTelemetryLine(sample));
                const auto ptp = state->ptp ? state->ptp->Summary() : gox::PtpSummary{};
                health.values["ptp"] = {
                    {"enabled", ptp.enabled}, {"synchronized", ptp.synchronized}, {"status", ptp.status}
                };
                if (state->counter_started) {
                    if (sample.trig && state->previous && *sample.trig < *state->previous) {
                        state->counter_regressed = true;
                    }
                    if (sample.trig) {
                        state->previous = sample.trig;
                    }
                    state->counter_binding_lost |= state->counter_bound && !sample.trig_binding_verified;
                    state->counter_status_unavailable |= !sample.trig_status_read;
                    state->counter_input_unverified |= !sample.trig_input_verified;
                    state->counter_value_unavailable |= !sample.trig;
                    state->counter_negative |= sample.trig && *sample.trig < 0;
                    state->counter_overflow |= sample.trig_overflow;
                }
                auto unknown = nlohmann::json::array();
                if (!state->counter_started) {
                    unknown.push_back("baseline_not_sampled");
                }
                if (!state->sensor_sync) {
                    unknown.push_back("no_sensor_sync_window");
                }
                if (state->camera.acquisition.trigger.mode != gox::TriggerMode::kExternal) {
                    unknown.push_back("not_external_trigger");
                }
                if (!state->counter_bound) {
                    unknown.push_back("counter_binding_not_verified_at_configure");
                }
                if (!state->counter_reset) {
                    unknown.push_back("reset_not_acknowledged");
                }
                if (!state->baseline) {
                    unknown.push_back("baseline_unavailable");
                }
                if (!sample.trig) {
                    unknown.push_back("current_value_unavailable");
                }
                if (state->counter_binding_lost) {
                    unknown.push_back("binding_lost");
                }
                if (state->counter_status_unavailable) {
                    unknown.push_back("status_unavailable");
                }
                if (state->counter_input_unverified) {
                    unknown.push_back("trigger_input_unverified");
                }
                if (state->counter_value_unavailable) {
                    unknown.push_back("value_unavailable_during_window");
                }
                if (state->counter_negative) {
                    unknown.push_back("negative_value");
                }
                if (state->counter_regressed) {
                    unknown.push_back("counter_regressed");
                }
                if (state->counter_overflow) {
                    unknown.push_back("counter_overflow");
                }
                if (unknown.empty())
                    health.triggered_frames = static_cast<std::uint64_t>(*sample.trig - *state->baseline);
                health.values["trigger_counter_baseline"] = state->baseline
                                                                ? nlohmann::json(*state->baseline)
                                                                : nlohmann::json(nullptr);
                health.values["trigger_counter_regressed"] = state->counter_regressed;
                health.values["trigger_counter_overflow_seen"] = state->counter_overflow;
                health.values["trigger_counter_accounting"] = {
                    {"reset_acknowledged", state->counter_reset},
                    {"binding_lost", state->counter_binding_lost},
                    {"status_unavailable", state->counter_status_unavailable},
                    {"negative_value_seen", state->counter_negative},
                    {"input_unverified", state->counter_input_unverified},
                    {
                        "triggered_frames",
                        health.triggered_frames ? nlohmann::json(*health.triggered_frames) : nlohmann::json(nullptr)
                    },
                    {"unknown_reasons", unknown}
                };
                if (state->counter_bound && (state->counter_binding_lost || state->counter_regressed || state->
                                             counter_overflow || state->counter_negative)) {
                    health.failure = "GOX trigger counter binding/reset/wrap invalidated the session";
                }
                const double hottest = std::max({
                    sample.temp_sensor.value_or(0.0), sample.temp_mainboard.value_or(0.0),
                    sample.temp_fpga.value_or(0.0)
                });
                if (!state->thermal_warning && hottest >= gox::kThermalLimitC) {
                    state->thermal_warning = true;
                    log.Warn("[{}] camera temperature {:.1f} C reached its documented limit", state->camera.id,
                             hottest);
                } else if (hottest <= gox::kThermalRearmC) {
                    state->thermal_warning = false;
                }
                return health;
            };
            definitions.push_back(std::move(definition));
        }
        if (definitions.empty()) {
            throw std::runtime_error("GOX configuration has no enabled cameras");
        }
        return definitions;
    }


    std::vector<CameraDefinition> LoadGoxCameras(const std::filesystem::path &config,
                                                 const std::filesystem::path &output) {
        return MakeGoxCameras(gox::LoadAppConfig(config.string()), config, output);
    }
} // namespace ebus_driver
