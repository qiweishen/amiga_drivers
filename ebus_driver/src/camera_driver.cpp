#include "ebus_driver/camera_driver.h"
#include "ebus_driver/block_ids.h"

#include <PvBuffer.h>
#include <PvDeviceGEV.h>
#include <PvGenCommand.h>
#include <PvGenInteger.h>
#include <PvGenParameterArray.h>
#include <PvImage.h>
#include <PvStreamGEV.h>
#include <PvVersion.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "bounded_queue.h"
#include "ebus/discovery.h"
#include "ebus/env_bootstrap.h"
#include "ebus/sdk_error.h"
#include "logger.h"
#include "sensor_sync_hub.h"
#include "sensor_sync_quality.h"
#include "time_util.h"

namespace ebus_driver {
    const char *SdkHeaderVersion() noexcept { return NVERSION_STRING; }

    std::string DeviceMetadataFormat() {
        return std::string("ebus-device-eBUS_SDK_v") + SdkHeaderVersion();
    }

    std::string RawRecordingFormat() {
        return std::string("ebus-raw-eBUS_SDK_v") + SdkHeaderVersion();
    }

    namespace {
        using Json = nlohmann::json;
        using Clock = std::chrono::steady_clock;
        using BufferPtr = std::unique_ptr<RawBuffer>;
        using common::Ebus::ToStd;

        class StopInterrupted final : public std::runtime_error {
        public:
            StopInterrupted() : std::runtime_error("camera preparation interrupted") {
            }
        };


        bool Accepted(const PvResult &result) {
            return result.IsOK() || result.GetCode() == PvResult::Code::PENDING;
        }


        Json ReadInteger(PvGenParameterArray *parameters, const char *name) {
            if (!parameters) return nullptr;
            auto *node = parameters->GetInteger(PvString(name));
            std::int64_t value = 0;
            return node && node->GetValue(value).IsOK() ? Json(value) : Json(nullptr);
        }


        void Command(PvDeviceGEV &device, const char *name) {
            auto *parameters = device.GetParameters();
            auto *command = parameters ? dynamic_cast<PvGenCommand *>(parameters->Get(PvString(name))) : nullptr;
            if (!command) throw std::runtime_error(std::string("missing command: ") + name);
            CHECK_PV(command->Execute(), name);
        }
    } // namespace


    struct EbusCameraDriver::Impl {
        EbusCameraDriver &owner;
        CameraDefinition definition;
        CameraOptions &options;
        DeviceHooks &hooks;
        std::shared_ptr<common::SensorSyncHub> sync;
        common::DriverLog log;
        StopPredicate external_stop;
        std::unique_ptr<PvDeviceGEV> device;
        std::unique_ptr<PvStreamGEV> stream;
        std::vector<std::unique_ptr<PvBuffer> > sdk_buffers;
        std::unique_ptr<RawRecorder> recorder;
        std::unique_ptr<common::BoundedQueue<BufferPtr> > queue;
        std::vector<BufferPtr> free_slots;
        std::mutex pool_mutex;
        std::thread receiver, writer;
        std::mutex start_mutex;
        std::condition_variable start_changed;
        std::atomic<bool> start_gate{false}, stop_requested{false}, source_stopped{false};
        std::atomic<bool> acquiring{false}, sync_running_seen{false};
        std::atomic<std::uint64_t> data_reference_ns{0};
        mutable std::mutex error_mutex;
        std::string reason, error;
        bool init_attempted = false, initialized = false, registered = false;
        bool recorder_open = false, start_attempted = false, acquisition_started = false;
        bool stream_enable_attempted = false, workers_stopped = false, shutdown_done = false;
        bool run_called = false;
        std::uint32_t payload_capacity = 0, queue_capacity = 0;
        std::uint64_t capture_start_ns = 0, acquisition_ack_ns = 0;
        Json runtime = Json::object(), timing = Json::object();
        Json trigger_accounting = {{"status", "not_assessed"}, {"triggered_frames", nullptr}};
        BlockIds ids;
        std::atomic<std::uint64_t> retrieved{0}, admitted{0}, valid_observed{0}, valid_admitted{0};
        std::atomic<std::uint64_t> operation_errors{0}, unknown_payloads{0}, invalid_images{0};
        std::atomic<std::uint64_t> gaps{0}, gap_events{0}, anomalies{0}, queue_drops{0};
        std::atomic<std::uint64_t> unsafe_buffers{0}, excluded{0}, unconfirmed{0}, requeue_errors{0};
        std::atomic<std::uint64_t> retrieve_timeouts{0}, aborted_buffers{0}, stream_dropped{0};
        std::atomic<std::uint64_t> drain_budget_exhausted{0};


        Impl(EbusCameraDriver &app, CameraDefinition value, std::shared_ptr<common::SensorSyncHub> hub)
            : owner(app), definition(std::move(value)), options(definition.options), hooks(definition.hooks),
              sync(std::move(hub)), log(options.module.empty() ? "eBUS" : options.module) {
            ids = BlockIds(options.extended_block_ids);
        }


        std::string SyncOwner() const { return options.kind + ":" + options.id; }


        bool Stopping() const {
            return stop_requested.load(std::memory_order_acquire) || owner.TerminateFlag().load(
                       std::memory_order_acquire) ||
                   (external_stop && external_stop());
        }


        void CheckStop() const {
            if (Stopping()) {
                throw StopInterrupted();
            }
        }


        void Stop(const std::string &why) {
            stop_requested.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(error_mutex);
            if (reason.empty()) {
                reason = why;
            }
        }


        void Fail(const std::string &why) noexcept {
            owner.RequestFailure();
            stop_requested.store(true, std::memory_order_release);
            try {
                {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (error.empty()) error = why;
                    reason = "error";
                }
                log.Error("[{}] {}", options.id, why);
            } catch (...) {
                /* failure remains sticky even if diagnostics allocate */
            }
        }


        std::string Reason() const {
            std::lock_guard<std::mutex> lock(error_mutex);
            return error.empty() ? reason : error;
        }


        void Event(Json event) {
            if (!recorder_open) {
                return;
            }
            if (!event.contains("host_realtime_ns")) {
                event["host_realtime_ns"] = common::TimeUtil::RealtimeNowNs();
            }
            if (!event.contains("host_monotonic_ns")) {
                event["host_monotonic_ns"] = common::TimeUtil::MonotonicNowNs();
            }
            event["camera_id"] = options.id;
            recorder->Event(event);
        }


        void TryEvent(Json event) noexcept {
            try { Event(std::move(event)); } catch (const std::exception &e) {
                Fail(std::string("event recording failed: ") + e.what());
            } catch (...) {
                Fail("event recording failed with an unknown exception");
            }
        }


        void OpenStream() {
            stream = std::make_unique<PvStreamGEV>();
            const std::uint64_t requested = static_cast<std::uint64_t>(options.socket_rx_buffer_mb) * 1024 * 1024;
            if (!requested || requested > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("socket_rx_buffer_mb exceeds the SDK's byte-size range");
            }
            const auto set = stream->SetUserModeSocketRxBufferSize(static_cast<std::uint32_t>(requested));
            runtime["socket_rx_requested_bytes"] = requested;
            runtime["socket_rx_set_result"] = common::Ebus::PvResultToString(set);
            const bool host_device_capped = set.GetCode() == PvResult::Code::INVALID_PARAMETER;
            runtime["socket_rx_sdk_reported_cap"] = host_device_capped;
            runtime["socket_rx_rmem_max_bytes"] = nullptr;
            if (!set.IsOK() && set.GetCode() != PvResult::Code::NOT_SUPPORTED) {
                if (host_device_capped) {
                    std::uint64_t limit = 0;
                    std::ifstream limit_file("/proc/sys/net/core/rmem_max");
                    const bool limit_known = (limit_file >> limit) && limit > 0 && limit <= std::numeric_limits<
                                                 std::uint32_t>::max();
                    if (limit_known) {
                        runtime["socket_rx_rmem_max_bytes"] = limit;
                    }
                    log.Warn(
                        "[{}] Requested socket RX buffer {} bytes exceeds the host Linux rmem_max limit; "
                        "the SDK has capped the request automatically (current rmem_max: {}); "
                        "actual socket size will be read after Open",
                        options.id, requested, limit_known ? std::to_string(limit) + " bytes" : "unavailable");
                } else {
                    throw common::Ebus::SdkError("SetUserModeSocketRxBufferSize", set);
                }
            }
            CHECK_PV(stream->Open(device->GetIPAddress(), 0, 0, PvString(options.local_ip.c_str())),
                     "PvStreamGEV::Open");
            std::uint32_t effective = 0;
            const auto read = stream->GetUserModeSocketRxBufferSize(effective);
            runtime["socket_rx_read_result"] = common::Ebus::PvResultToString(read);
            runtime["socket_rx_effective_bytes"] = read.IsOK() ? Json(effective) : Json(nullptr);
            runtime["socket_rx_semantics"] =
                    "SDK SO_RCVBUF readback; Linux may include doubled bookkeeping; not payload capacity";
            if (read.IsOK() && effective == 0) {
                throw std::runtime_error("SDK receive socket capacity readback is zero");
            }
            if (read.IsOK() && !host_device_capped && effective < requested) {
                throw std::runtime_error("SDK receive socket capacity is below the requested size");
            }
            if (!read.IsOK() && read.GetCode() != PvResult::Code::NOT_SUPPORTED) {
                throw common::Ebus::SdkError("GetUserModeSocketRxBufferSize", read);
            }
            if (host_device_capped && read.IsOK()) {
                log.Info("[{}] Capped socket RX buffer readback: {} bytes (requested {} bytes; "
                         "SDK value may include kernel bookkeeping)", options.id, effective, requested);
            }
            if (options.packet_size) {
                CHECK_PV(device->SetPacketSize(options.packet_size, 0), "PvDeviceGEV::SetPacketSize");
            } else {
                const auto negotiated = device->NegotiatePacketSize(0);
                if (!negotiated.IsOK()) {
                    log.Warn("[{}] packet negotiation failed; requesting 1476 bytes", options.id);
                    CHECK_PV(device->SetPacketSize(1476, 0), "PvDeviceGEV::SetPacketSize fallback");
                }
            }
            CHECK_PV(device->SetStreamDestination(stream->GetLocalIPAddress(), stream->GetLocalPort(), 0),
                     "PvDeviceGEV::SetStreamDestination");
            if (hooks.configure_stream) {
                hooks.configure_stream(*stream);
            }
            runtime["stream_local_ip"] = ToStd(stream->GetLocalIPAddress());
            runtime["stream_local_port"] = stream->GetLocalPort();
            runtime["packet_size"] = ReadInteger(device->GetParameters(), "GevSCPSPacketSize");
        }


        void Allocate() {
            payload_capacity = device->GetPayloadSize();
            if (payload_capacity == 0) {
                throw std::runtime_error("camera reports zero PayloadSize");
            }
            if (!std::isfinite(options.expected_rate_hz) || options.expected_rate_hz <= 0 ||
                !std::isfinite(options.stall_budget_s) || options.stall_budget_s < 0 ||
                !std::isfinite(options.max_duration_s) || options.max_duration_s < 0 ||
                !std::isfinite(options.telemetry_interval_s) || options.telemetry_interval_s <= 0 ||
                !std::isfinite(options.statistics_interval_s) || options.statistics_interval_s < 0 ||
                options.retrieve_timeout_ms == 0 || options.retrieve_timeout_ms > 10000)
                throw std::runtime_error("invalid rate, duration, telemetry or retrieve timeout");
            const std::uint64_t memory = static_cast<std::uint64_t>(options.max_buffer_memory_mb) * 1024 * 1024;
            const std::uint64_t slots = memory / payload_capacity;
            // Four SDK buffers and two queued images plus producer/writer ownership.
            if (slots < 8) {
                throw std::runtime_error("buffer memory limit cannot fit four SDK and four application buffers");
            }
            const std::uint32_t sdk_max = stream->GetQueuedBufferMaximum();
            std::uint64_t sdk_count = std::max<std::uint64_t>(4, options.sdk_buffers);
            if (sdk_max) {
                sdk_count = std::min<std::uint64_t>(sdk_count, sdk_max);
            }
            sdk_count = std::min<std::uint64_t>(sdk_count, slots - 4);
            if (sdk_count < 4) {
                throw std::runtime_error("SDK queue maximum is below four buffers");
            }
            const double desired = std::ceil(options.expected_rate_hz * options.stall_budget_s);
            if (!std::isfinite(desired) || desired > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("configured queue stall budget is too large");
            }
            const std::uint64_t wanted_queue = options.queue_frames
                                                   ? options.queue_frames
                                                   : std::max<std::uint64_t>(2, static_cast<std::uint64_t>(desired));
            if (wanted_queue < 2) {
                throw std::runtime_error("recording queue needs at least two buffers");
            }
            queue_capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(wanted_queue, slots - sdk_count - 2));
            queue = std::make_unique<common::BoundedQueue<BufferPtr> >(queue_capacity);
            free_slots.reserve(static_cast<std::size_t>(queue_capacity) + 2);
            for (std::size_t i = 0; i < static_cast<std::size_t>(queue_capacity) + 2; ++i) {
                auto slot = std::make_unique<RawBuffer>();
                slot->payload.reserve(payload_capacity);
                free_slots.push_back(std::move(slot));
            }
            sdk_buffers.reserve(static_cast<std::size_t>(sdk_count));
            for (std::uint64_t i = 0; i < sdk_count; ++i) {
                auto buffer = std::make_unique<PvBuffer>();
                CHECK_PV(buffer->Alloc(payload_capacity), "PvBuffer::Alloc");
                sdk_buffers.push_back(std::move(buffer));
            }
            for (auto &buffer: sdk_buffers) {
                const auto result = stream->QueueBuffer(buffer.get());
                if (!Accepted(result)) {
                    throw common::Ebus::SdkError("initial QueueBuffer", result);
                }
            }
            runtime["payload_capacity_bytes"] = payload_capacity;
            // Device-specific raw overrides are applied after OpenStream. Retain
            // the final packet-size readback alongside the actual allocations.
            runtime["packet_size"] = ReadInteger(device->GetParameters(), "GevSCPSPacketSize");
            runtime["sdk_buffer_count"] = sdk_count;
            runtime["recording_queue_frames"] = queue_capacity;
            runtime["recording_pool_slots"] = static_cast<std::uint64_t>(queue_capacity) + 2;
            runtime["payload_buffer_bytes"] = (sdk_count + queue_capacity + 2) * payload_capacity;
            runtime["memory_budget_scope"] =
                    "SDK and application payload allocations; excludes SDK internals and JSON metadata";
            runtime["estimated_queue_stall_seconds"] = queue_capacity / options.expected_rate_hz;
            runtime["estimated_rate_hz"] = options.expected_rate_hz;
            runtime["queue_was_clamped"] = queue_capacity != wanted_queue;
            runtime["sdk_buffers_were_clamped"] = sdk_count != options.sdk_buffers;
            runtime["queue_full_policy"] =
                    "fail session; preserve accepted buffers and record rejected buffer identity";
            runtime["block_id_mode"] = options.extended_block_ids ? "64-bit extended" : "16-bit cycle 1..65535";
        }


        Json DeviceMetadata() {
            Json metadata = hooks.metadata ? hooks.metadata(*device) : Json::object();
            if (!metadata.is_object()) {
                throw std::runtime_error("device profile metadata must be an object");
            }
            metadata["format"] = DeviceMetadataFormat();
            metadata["kind"] = options.kind;
            metadata["id"] = options.id;
            metadata["recording_format"] = RawRecordingFormat();
            metadata["runtime"] = runtime;
            metadata["sdk"] = {
                {"name", "Pleora eBUS"}, {"header_version", SdkHeaderVersion()},
                {"header_version_source", "PvVersion.h NVERSION_STRING at build time"},
                {"runtime_version", nullptr}, {"runtime_version_status", "not queried; never inferred from headers"}
            };
            metadata["recording_contract"] = {
                {"payload", "unchanged GetAcquiredSize bytes copied from PvBuffer::GetDataPointer before requeue"},
                {"device_timestamp", "unscaled PvBuffer::GetTimestamp"},
                {"device_timestamp_unit", "unverified"}, {"device_timestamp_epoch", "unverified"},
                {"host_timestamp_semantics", "sampled immediately after RetrieveBuffer; reception, not exposure time"},
                {
                    "processing",
                    "no unpacking, cropping, status-line removal, demosaic, calibration or synthesized pixels"
                },
                {
                    "limit_semantics",
                    "max_duration starts immediately before AcquisitionStart; max_frames counts admitted valid image buffers"
                }
            };
            metadata["sensor_sync"] = {
                {"enabled", options.sensor_sync},
                {"channel", options.sensor_sync ? Json(options.sensor_channel) : Json(nullptr)},
                {"pulse_rate_hz", options.sensor_sync ? Json(options.pulse_hz) : Json(nullptr)},
                {"observations_file", options.sensor_sync && sync ? Json(sync->LogPath().string()) : Json(nullptr)},
                {"association_verified", false}
            };
            return metadata;
        }


        bool Init(const StopPredicate &predicate) {
            if (init_attempted) {
                Fail("Init called twice on a single-use camera driver");
                return false;
            }
            init_attempted = true;
            external_stop = predicate;
            try {
                CheckStop();
                if (options.id.empty() || options.kind.empty()) {
                    throw std::runtime_error("camera id and kind are required");
                }
                common::Ebus::BootstrapEnv();
                std::string target = options.ip;
                if (!options.mac.empty()) {
                    target = common::Ebus::FindCamera(options.mac).connection_id;
                }
                CheckStop();
                if (target.empty()) {
                    throw std::runtime_error("camera MAC or IP is required");
                }
                device = std::make_unique<PvDeviceGEV>();
                CHECK_PV(device->Connect(PvString(target.c_str()), PvAccessControl), "PvDeviceGEV::Connect");
                const StopPredicate stopping = [this] { return Stopping(); };
                if (hooks.prepare) {
                    hooks.prepare(*device, stopping);
                }
                CheckStop();
                OpenStream();
                if (hooks.configure) {
                    hooks.configure(*device, stopping);
                }
                CheckStop();
                // The profile default is only a fallback for older devices without
                // this node. Never infer a 64-bit cycle from an arbitrary large ID.
                auto *parameters = device->GetParameters();
                auto *extended = parameters ? parameters->Get(PvString("GevGVSPExtendedIDMode")) : nullptr;
                runtime["block_id_mode_source"] = "profile fallback; GevGVSPExtendedIDMode absent";
                if (extended) {
                    PvString value;
                    CHECK_PV(extended->ToString(value), "read GevGVSPExtendedIDMode");
                    const auto text = ToStd(value);
                    bool mode = false;
                    if (text == "On" || text == "true" || text == "True" || text == "1") {
                        mode = true;
                    } else if (text == "Off" || text == "false" || text == "False" || text == "0") {
                        mode = false;
                    } else {
                        throw std::runtime_error("unrecognized GevGVSPExtendedIDMode readback: " + text);
                    }
                    ids = BlockIds(mode);
                    options.extended_block_ids = mode;
                    runtime["block_id_mode_source"] = "GevGVSPExtendedIDMode readback";
                    runtime["extended_block_ids_readback"] = text;
                }
                Allocate();
                recorder = std::make_unique<RawRecorder>(options.recording);
                recorder->Open(DeviceMetadata());
                recorder_open = true;
                if (options.sensor_sync) {
                    if (!sync) {
                        throw std::runtime_error("SensorSync enabled but no shared hub was provided");
                    }
                    sync->Register(SyncOwner(), options.sensor_channel, options.pulse_hz);
                    registered = true;
                }
                CheckStop();
                initialized = true;
                Event({{"event", "prepared"}, {"acquisition_started", false}});
                log.Info("[{}] Prepared; Output {}", options.id, options.recording.directory.string());
                return true;
            } catch (const StopInterrupted &e) {
                Stop("initialization interrupted");
                TryEvent({{"event", "initialization_interrupted"}, {"detail", e.what()}});
            } catch (const std::exception &e) {
                // An operator stop must not hide a simultaneous SDK or file error.
                Fail(std::string("Initialization failed: ") + e.what());
            } catch (...) {
                Fail("Initialization failed with an unknown exception");
            }
            // The coordinator still calls Shutdown, including for partial Init.
            return false;
        }


        BufferPtr AcquireSlot() {
            std::lock_guard<std::mutex> lock(pool_mutex);
            if (free_slots.empty()) {
                return {};
            }
            auto slot = std::move(free_slots.back());
            free_slots.pop_back();
            return slot;
        }


        void ReleaseSlot(BufferPtr slot) {
            if (!slot) {
                return;
            }
            slot->payload.clear();
            slot->metadata = Json::object();
            std::lock_guard<std::mutex> lock(pool_mutex);
            free_slots.push_back(std::move(slot));
        }


        void Requeue(PvBuffer *buffer) noexcept {
            try {
                const auto result = stream->QueueBuffer(buffer);
                if (!Accepted(result)) {
                    ++requeue_errors;
                    Fail("QueueBuffer failed: " + common::Ebus::PvResultToString(result));
                }
            } catch (const std::exception &e) {
                ++requeue_errors;
                Fail(std::string("QueueBuffer threw: ") + e.what());
            } catch (...) {
                ++requeue_errors;
                Fail("QueueBuffer threw an unknown exception");
            }
        }


        void Process(PvBuffer *buffer, const PvResult &operation, std::uint64_t host_rt, std::uint64_t host_mono) {
            if (!buffer) {
                Fail("RetrieveBuffer succeeded with a null buffer");
                return;
            }
            // SDK buffer ownership is returned on every path, including exceptions
            // while JSON/payload storage is being prepared. Normal requeue precedes
            // queue publication and therefore never waits for downstream disk I/O.
            struct ReturnBuffer {
                Impl &self;
                PvBuffer *buffer;
                ~ReturnBuffer() { if (buffer) self.Requeue(buffer); }

                void Now() {
                    if (buffer) {
                        auto *value = buffer;
                        buffer = nullptr;
                        self.Requeue(value);
                    }
                }
            } returned{*this, buffer};
            // AbortQueuedBuffers is issued only after this worker has joined.
            // An ABORTED result here is therefore an observed error buffer, not an
            // unused allocation returned by our own teardown. Preserve its safe bytes.
            if (operation.GetCode() == PvResult::Code::ABORTED) {
                ++aborted_buffers;
            }
            ++retrieved;
            const std::uint64_t acquired = buffer->GetAcquiredSize();
            const auto *source = buffer->GetDataPointer();
            const auto observation = ids.Observe(buffer->GetBlockID());
            if (observation.gap) {
                gaps.fetch_add(observation.gap);
                ++gap_events;
            }
            if (observation.anomaly) ++anomalies;
            Json image_metadata = nullptr;
            bool image_complete = false;
            const bool safe = acquired <= payload_capacity && (acquired == 0 || source != nullptr);
            if (buffer->GetPayloadType() == PvPayloadTypeImage) {
                if (auto *image = buffer->GetImage()) {
                    const std::uint64_t image_size = image->GetImageSize();
                    const std::uint64_t effective_size = image->GetEffectiveImageSize();
                    const auto *image_data = image->GetDataPointer();
                    Json offset = nullptr;
                    if (safe && source && image_data) {
                        const auto base = reinterpret_cast<std::uintptr_t>(source);
                        const auto address = reinterpret_cast<std::uintptr_t>(image_data);
                        if (address >= base && address - base <= acquired && image_size <= acquired - (address - base))
                            offset = static_cast<std::uint64_t>(address - base);
                    }
                    const bool partial = image->IsPartialLineMissing();
                    const bool full = image->IsFullLineMissing();
                    const bool overrun = image->IsDataOverrun();
                    image_metadata = {
                        {"pixel_format", static_cast<std::uint32_t>(image->GetPixelType())},
                        {"width", image->GetWidth()}, {"height", image->GetHeight()},
                        {"offset_x", image->GetOffsetX()}, {"offset_y", image->GetOffsetY()},
                        {"padding_x", image->GetPaddingX()}, {"padding_y", image->GetPaddingY()},
                        {"image_size", image_size}, {"effective_image_size", effective_size},
                        {"payload_offset", offset},
                        {"partial_line_missing", partial}, {"full_line_missing", full}, {"data_overrun", overrun}
                    };
                    image_complete = !offset.is_null() && image->GetWidth() != 0 && image->GetHeight() != 0 &&
                                     image_size != 0 && effective_size != 0 && effective_size <= image_size && !partial
                                     && !full && !overrun;
                }
                if (!image_complete) {
                    ++invalid_images;
                }
            } else {
                ++unknown_payloads;
            }
            const bool valid = operation.IsOK() && safe && image_complete && !observation.anomaly;
            Json metadata = {
                {"record_kind", "buffer"}, {"block_id", buffer->GetBlockID()},
                {"device_timestamp_raw", buffer->GetTimestamp()}, {"host_realtime_ns", host_rt},
                {"host_monotonic_ns", host_mono}, {"acquired_size", acquired},
                {"payload_type", static_cast<std::uint32_t>(buffer->GetPayloadType())},
                {"operation_result", static_cast<std::uint32_t>(operation.GetCode())},
                {"operation_ok", operation.IsOK()},
                {"operation_name", ToStd(operation.GetCodeString())}, {"chunk_count", buffer->GetChunkCount()},
                {"valid_frame", valid}, {"block_id_anomaly", observation.anomaly}, {"gap_before", observation.gap},
                {"image", std::move(image_metadata)}
            };
            if (valid) {
                ++valid_observed;
                data_reference_ns.store(host_mono, std::memory_order_release);
            }
            if (!operation.IsOK()) {
                ++operation_errors;
                Fail("SDK delivered an operation-error buffer; safe bytes retained");
            }
            if (observation.gap || observation.anomaly) Fail("GVSP BlockID discontinuity; received payload retained");
            if (buffer->GetPayloadType() == PvPayloadTypeImage && !image_complete)
                Fail("SDK image is incomplete or its image extent cannot be proved within the acquired payload");
            if (!safe) {
                ++unsafe_buffers;
                metadata["record_kind"] = "unsafe_buffer_rejected";
                metadata["payload_capacity_bytes"] = payload_capacity;
                metadata["reason"] =
                        "acquired_size exceeds allocation or nonempty data pointer is null; no bytes dereferenced";
                returned.Now();
                TryEvent(std::move(metadata));
                Fail("unsafe SDK payload extent; buffer identity recorded without reading payload");
                return;
            }
            if (valid && options.max_frames && valid_admitted.load() >= options.max_frames) {
                ++excluded;
                metadata["record_kind"] = "limit_excluded_buffer";
                returned.Now();
                TryEvent(std::move(metadata));
                return;
            }
            auto slot = AcquireSlot();
            if (!slot) {
                ++queue_drops;
                metadata["record_kind"] = "queue_rejected_buffer";
                returned.Now();
                TryEvent(std::move(metadata));
                Fail("recording buffer pool exhausted");
                return;
            }
            slot->metadata = std::move(metadata);
            slot->payload.resize(static_cast<std::size_t>(acquired));
            if (acquired) {
                std::memcpy(slot->payload.data(), source, static_cast<std::size_t>(acquired));
            }
            returned.Now();
            if (!queue->try_push(std::move(slot))) {
                ++queue_drops;
                auto rejected = slot->metadata;
                rejected["record_kind"] = "queue_rejected_buffer";
                ReleaseSlot(std::move(slot));
                TryEvent(std::move(rejected));
                Fail("recording queue full; accepted buffers will drain");
                return;
            }
            ++admitted;
            if (valid) {
                const auto count = ++valid_admitted;
                if (options.max_frames && count >= options.max_frames) Stop("frame limit");
            }
        }


        void Receive() noexcept {
            try {
                {
                    std::unique_lock<std::mutex> lock(start_mutex);
                    start_changed.wait(lock, [this] { return start_gate.load() || source_stopped.load(); });
                }
                if (start_gate.load()) {
                    std::optional<Clock::time_point> draining;
                    auto last_arrival = Clock::now();
                    for (;;) {
                        if (source_stopped.load(std::memory_order_acquire) && !draining) {
                            draining = Clock::now();
                            last_arrival = *draining;
                        }
                        if (draining && Clock::now() - *draining >= std::chrono::seconds(5)) {
                            ++drain_budget_exhausted;
                            Fail(
                                "post-stop receive drain exceeded five seconds; remaining SDK buffers are unconfirmed");
                            break;
                        }
                        PvBuffer *buffer = nullptr;
                        PvResult operation;
                        const auto result = stream->RetrieveBuffer(&buffer, &operation,
                                                                   draining ? 200 : options.retrieve_timeout_ms);
                        const auto host_rt = common::TimeUtil::RealtimeNowNs();
                        const auto host_mono = common::TimeUtil::MonotonicNowNs();
                        if (!result.IsOK()) {
                            if (result.GetCode() == PvResult::Code::TIMEOUT) {
                                ++retrieve_timeouts;
                                if (draining && Clock::now() - last_arrival >= std::chrono::seconds(1)) {
                                    break;
                                }
                                continue;
                            }
                            if (result.GetCode() == PvResult::Code::ABORTED && source_stopped.load()) {
                                break;
                            }
                            Fail("RetrieveBuffer failed: " + common::Ebus::PvResultToString(result));
                            break;
                        }
                        last_arrival = Clock::now();
                        Process(buffer, operation, host_rt, host_mono);
                    }
                }
            } catch (const std::exception &e) {
                Fail(std::string("receive worker failed: ") + e.what());
            } catch (...) {
                Fail("receive worker failed with an unknown exception");
            }
            acquiring.store(false, std::memory_order_release);
            data_reference_ns.store(0, std::memory_order_release);
            queue->close();
        }


        void Write() noexcept {
            bool io_failed = false;
            BufferPtr slot;
            try {
                while (queue->pop(slot)) {
                    if (!io_failed) {
                        try { recorder->Append(*slot); } catch (const std::exception &e) {
                            io_failed = true;
                            Fail(std::string("raw writer failed: ") + e.what());
                        } catch (...) {
                            io_failed = true;
                            Fail("raw writer failed with an unknown exception");
                        }
                    }
                    if (io_failed) {
                        ++unconfirmed;
                    }
                    ReleaseSlot(std::move(slot));
                }
            } catch (const std::exception &e) {
                if (slot) {
                    ++unconfirmed;
                }
                Fail(std::string("writer worker failed: ") + e.what());
                // Stop further admissions. Records already queued remain explicitly
                // unconfirmed and are released below without touching the disk.
                queue->close();
                while (queue->pop(slot)) {
                    ++unconfirmed;
                    slot.reset();
                }
            } catch (...) {
                if (slot) {
                    ++unconfirmed;
                }
                Fail("writer worker failed with an unknown exception");
                queue->close();
                while (queue->pop(slot)) {
                    ++unconfirmed;
                    slot.reset();
                }
            }
        }


        void Sample(const char *phase) {
            if (!hooks.sample || !device || !device->IsConnected()) {
                return;
            }
            const auto sample = hooks.sample(*device);
            Event({
                {"event", "device_health"}, {"phase", phase}, {"values", sample.values}, {"failure", sample.failure}
            });
            if (!sample.failure.empty()) {
                Fail(sample.failure);
            }
            if (std::string_view(phase) == "stop") {
                const bool bounded = registered && sync && sync->Started() && sync->Ok() && options.pulse_hz > 0;
                trigger_accounting = {
                    {"status", "unavailable"},
                    {"triggered_frames", sample.triggered_frames ? Json(*sample.triggered_frames) : Json(nullptr)},
                    {"source_bounded", bounded}, {"valid_observed_frames", valid_observed.load()},
                    {"gap_missing_frames", gaps.load()}, {"emitted_frames", nullptr},
                    {"counts_match", nullptr}, {"association_verified", false}
                };
                if (bounded && sample.triggered_frames) {
                    const auto observed = valid_observed.load();
                    const auto missing = gaps.load();
                    if (missing > std::numeric_limits<std::uint64_t>::max() - observed) {
                        trigger_accounting["status"] = "count_overflow";
                        Fail("Trigger/Frame accounting overflow");
                    } else {
                        const auto emitted = observed + missing;
                        const bool matches = emitted == *sample.triggered_frames;
                        trigger_accounting["emitted_frames"] = emitted;
                        trigger_accounting["counts_match"] = matches;
                        trigger_accounting["status"] = matches ? "counts_match" : "count_mismatch";
                        if (!matches) {
                            Fail("Camera trigger counter does not match emitted frames; Trailing/Leading loss is possible");
                        }
                    }
                }
                Event({{"event", "trigger_counter_accounting"}, {"accounting", trigger_accounting}});
            }
        }


        void ObserveSyncStart() {
            if (registered && sync->Started() && !sync_running_seen.load(std::memory_order_acquire)) {
                // Waiting for another participant to arm is outside the camera's
                // no-data budget. Publish the new reference before enabling the guard.
                if (valid_observed.load() == 0) {
                    data_reference_ns.store(common::TimeUtil::MonotonicNowNs(), std::memory_order_release);
                }
                sync_running_seen.store(true, std::memory_order_release);
            }
        }


        void StreamStatistics(bool final) {
            if (!stream || !stream->IsOpen()) {
                return;
            }
            auto *parameters = stream->GetParameters();
            const auto dropped = ReadInteger(parameters, "BlocksDropped");
            if (dropped.is_number_integer() && dropped.get<std::int64_t>() > 0) {
                const auto count = dropped.get<std::uint64_t>();
                if (count > stream_dropped.exchange(count)) {
                    Fail("SDK stream reports dropped blocks");
                }
            }
            if (final) {
                Json values = Json::object();
                if (parameters) {
                    for (std::uint32_t i = 0; i < parameters->GetCount(); ++i) {
                        auto *parameter = parameters->Get(i);
                        if (!parameter) {
                            continue;
                        }
                        PvString name, value;
                        if (!parameter->GetName(name).IsOK()) {
                            continue;
                        }
                        values[ToStd(name)] = parameter->IsReadable() && parameter->ToString(value).IsOK()
                                                  ? Json(ToStd(value))
                                                  : Json(nullptr);
                    }
                }
                Event({{"event", "stream_final_statistics"}, {"values", std::move(values)}});
            }
        }


        void StopWorkers() noexcept {
            if (workers_stopped) return;
            workers_stopped = true;
            stop_requested.store(true, std::memory_order_release);
            if (registered && sync) {
                try { sync->Disarm(SyncOwner()); } catch (...) { Fail("SensorSync disarm failed"); }
            }
            if (start_attempted && device) {
                try {
                    if (!device->IsConnected())
                        throw std::runtime_error(
                            "control link disconnected before AcquisitionStop");
                    Command(*device, "AcquisitionStop");
                } catch (const std::exception &e) {
                    Fail(std::string("AcquisitionStop was not acknowledged: ") + e.what());
                } catch (...) { Fail("AcquisitionStop failed with an unknown exception"); }
            }
            {
                std::lock_guard<std::mutex> lock(start_mutex);
                source_stopped.store(true, std::memory_order_release);
            }
            start_changed.notify_all();
            if (receiver.joinable()) {
                receiver.join();
            }
            if (queue) {
                queue->close();
            }
            if (writer.joinable()) {
                writer.join();
            }
            acquiring.store(false, std::memory_order_release);
            data_reference_ns.store(0, std::memory_order_release);
        }


        void Run() {
            if (!initialized || run_called) {
                Fail("Run requires one successfully prepared camera and is single-use");
                return;
            }

            run_called = true;
            try {
                CheckStop();
                // Worker creation cannot leave the source acquiring without a writer.
                writer = std::thread([this] { Write(); });
                receiver = std::thread([this] { Receive(); });
                const StopPredicate stopping = [this] { return Stopping(); };
                if (hooks.before_start) {
                    auto actions = hooks.before_start(*device, stopping);
                    Event({{"event", "profile_start"}, {"actions", std::move(actions)}});
                }
                CheckStop();
                if (device->GetPayloadSize() != payload_capacity) {
                    throw std::runtime_error(
                        "PayloadSize changed after preparation; refusing to start with stale allocations");
                }
                stream_enable_attempted = true;
                CHECK_PV(device->StreamEnable(), "PvDeviceGEV::StreamEnable");
                CheckStop();
                capture_start_ns = common::TimeUtil::MonotonicNowNs();
                start_attempted = true;
                Command(*device, "AcquisitionStart");
                acquisition_ack_ns = common::TimeUtil::MonotonicNowNs();
                acquisition_started = true;
                data_reference_ns.store(capture_start_ns, std::memory_order_release);
                acquiring.store(true, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(start_mutex);
                    start_gate.store(true, std::memory_order_release);
                }
                start_changed.notify_all();
                if (registered) {
                    sync->Arm(SyncOwner());
                }
                ObserveSyncStart();
                Event({
                    {"event", "acquisition_started"}, {"command_begin_monotonic_ns", capture_start_ns},
                    {"command_ack_monotonic_ns", acquisition_ack_ns}
                });
                Sample("start");
                log.Info("[{}] Acquisition started; Recording raw SDK buffers", options.id);
                auto next_sample = Clock::now() + std::chrono::duration<double>(options.telemetry_interval_s);
                auto last_stats_ns = capture_start_ns;
                std::uint64_t last_written = 0, last_observed = 0;
                while (!Stopping()) {
                    StreamStatistics(false);
                    if (!device->IsConnected()) {
                        Fail("Camera control link disconnected");
                    }
                    const auto now_ns = common::TimeUtil::MonotonicNowNs();
                    if (options.max_duration_s > 0 && static_cast<double>(now_ns - capture_start_ns) >= options.
                        max_duration_s * 1e9) {
                        Stop("duration limit");
                    }
                    if (registered) {
                        ObserveSyncStart();
                        if (!sync->Ok()) {
                            Fail("SensorSync failed: " + sync->LastError());
                        }
                        else if (sync->StalledSeconds() > 15) {
                            Fail("SensorSync timing log stalled for over 15 seconds");
                        }
                        else if (sync->WaitingSeconds(SyncOwner()) > 30) {
                            Fail("SensorSync participants did not all arm within 30 seconds");
                        }
                        else if (sync->Ended() && !Stopping()) Stop("Shared SensorSync session ended");
                    }
                    if (Clock::now() >= next_sample) {
                        Sample("capture");
                        next_sample = Clock::now() + std::chrono::duration<double>(options.telemetry_interval_s);
                    }
                    if (options.statistics_interval_s > 0 &&
                        static_cast<double>(now_ns - last_stats_ns) >= options.statistics_interval_s * 1e9) {
                        const double seconds = static_cast<double>(now_ns - last_stats_ns) / 1e9;
                        const auto written = recorder->FramesWritten();
                        const auto observed = valid_observed.load();
                        log.Info(
                            "[Statistics] [{}] up={}s rate={:.1f} Hz  fps={:.1f}  frames={} buffers={} lost={} queue={}/{}",
                            options.id, (now_ns - capture_start_ns) / 1000000000ull,
                            static_cast<double>(observed - last_observed) / seconds,
                            static_cast<double>(written - last_written) / seconds,
                            written, recorder->RecordsWritten(), gaps.load(), queue->size(), queue_capacity);
                        last_stats_ns = now_ns;
                        last_written = written;
                        last_observed = observed;
                    }
                    if (!Stopping()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (Reason().empty()) {
                    Stop("external stop");
                }
            } catch (const StopInterrupted &) {
                Stop("start interrupted");
            } catch (const std::exception &e) {
                Fail(std::string("camera run failed: ") + e.what());
            } catch (...) { Fail("camera run failed with an unknown exception"); }
            StopWorkers();
        }


        Json Counters() const {
            return {
                {"buffers_retrieved", retrieved.load()}, {"buffers_admitted", admitted.load()},
                {"valid_frames_observed", valid_observed.load()}, {"valid_frames_admitted", valid_admitted.load()},
                {"operation_errors", operation_errors.load()}, {"non_image_payloads", unknown_payloads.load()},
                {"invalid_images", invalid_images.load()}, {"frames_lost_gap", gaps.load()},
                {"block_id_gap_events", gap_events.load()}, {"block_id_anomalies", anomalies.load()},
                {"recording_queue_drops", queue_drops.load()}, {"unsafe_buffers", unsafe_buffers.load()},
                {"frames_limit_excluded", excluded.load()}, {"write_unconfirmed", unconfirmed.load()},
                {"requeue_errors", requeue_errors.load()}, {"retrieve_timeouts", retrieve_timeouts.load()},
                {"aborted_buffers", aborted_buffers.load()}, {"stream_blocks_dropped", stream_dropped.load()},
                {"drain_budget_exhausted", drain_budget_exhausted.load()}
            };
        }

        void Shutdown() noexcept {
            if (shutdown_done) {
                return;
            }
            StopWorkers();
            if (acquisition_started && options.expect_data && valid_observed.load() == 0) {
                Fail("capture ended without a valid image although this mode requires incoming frames");
            }
            // Each final operation has its own boundary: one failed sidecar or SDK
            // command must not skip recorder closure or release live SDK buffers.
            try {
                if (acquisition_started) Sample("stop");
            } catch (const std::exception &e) {
                Fail(std::string("final device sample failed: ") + e.what());
            } catch (...) { Fail("final device sample failed"); }
            try { StreamStatistics(true); } catch (const std::exception &e) {
                Fail(std::string("final stream statistics failed: ") + e.what());
            } catch (...) { Fail("final stream statistics failed"); }
            if (registered && sync) {
                try {
                    if (acquisition_started && sync->Started()) {
                        const auto quality = common::InspectSensorSync(sync->LogPath(),
                                                                       static_cast<unsigned>(options.sensor_channel),
                                                                       options.pulse_hz > 0, valid_observed.load(),
                                                                       gaps.load());
                        timing = quality.Json();
                        timing["frames_observed"] = valid_observed.load();
                        timing["frames_written"] = recorder ? recorder->FramesWritten() : 0;
                        timing["frames_limit_excluded"] = excluded.load();
                        timing["count_scope"] =
                                "valid observed frames, including deliberate max_frames drain exclusions";
                        timing["capture_window_bounded"] = options.pulse_hz > 0;
                        if (options.pulse_hz > 0) {
                            if (!quality.Ok() || !sync->Ok()) {
                                Fail("SensorSync exposure/frame accounting is incomplete");
                            }
                        } else {
                            // Freerun starts before Arm and stops after Disarm. Its
                            // capture interval cannot equal the edge-log interval;
                            // a boundary may legitimately bisect one exposure.
                            const bool healthy = quality.readable && quality.protocol_complete &&
                                                 quality.polarity_supported && quality.exposure.seen && quality.exposure
                                                 .valid && sync->Ok();
                            timing["ok"] = healthy;
                            timing["status"] = "observation_window_only";
                            timing["counts_reconciled"] = nullptr;
                            timing["count_scope"] =
                                    "freerun capture and SensorSync log have different boundaries; frame/edge count comparison unavailable";
                            if (!healthy) {
                                Fail("SensorSync freerun observation log is incomplete or malformed");
                            }
                        }
                    } else {
                        timing = {{"status", "not_started"}, {"association_verified", false}};
                        if (acquisition_started) {
                            Fail("camera acquired without its required SensorSync session");
                        }
                    }
                    Event({{"event", "sensor_sync_quality"}, {"quality", timing}});
                } catch (const std::exception &e) {
                    Fail(std::string("SensorSync final accounting failed: ") + e.what());
                } catch (...) { Fail("SensorSync final accounting failed"); }
            }
            if (stream) {
                try {
                    if (stream_enable_attempted && device) {
                        const auto result = device->StreamDisable();
                        if (!result.IsOK()) {
                            Fail("StreamDisable failed: " + common::Ebus::PvResultToString(result));
                        }
                    }
                    if (stream->IsOpen()) {
                        const auto aborted = stream->AbortQueuedBuffers();
                        if (!aborted.IsOK()) {
                            Fail("AbortQueuedBuffers failed: " + common::Ebus::PvResultToString(aborted));
                        }
                        // A finite attempt count protects teardown if the SDK cannot
                        // return all buffers. Close/destroy the stream before freeing any.
                        const auto deadline = Clock::now() + std::chrono::seconds(5);
                        while (stream->GetQueuedBufferCount() && Clock::now() < deadline) {
                            PvBuffer *buffer = nullptr;
                            PvResult operation;
                            const auto result = stream->RetrieveBuffer(&buffer, &operation, 200);
                            if (!result.IsOK()) {
                                Fail("could not retrieve all aborted SDK buffers");
                                break;
                            }
                            if (operation.GetCode() != PvResult::Code::ABORTED) {
                                Fail("completed SDK buffer remained after the receive drain; not recorded");
                                TryEvent({
                                    {"event", "undrained_sdk_buffer"},
                                    {"block_id", buffer ? Json(buffer->GetBlockID()) : Json(nullptr)},
                                    {"operation_name", ToStd(operation.GetCodeString())}
                                });
                            }
                        }
                        if (stream->GetQueuedBufferCount()) {
                            Fail("SDK buffers remained queued at stream close");
                        }
                        const auto closed = stream->Close();
                        if (!closed.IsOK()) {
                            Fail("stream close failed: " + common::Ebus::PvResultToString(closed));
                        }
                    }
                } catch (const std::exception &e) {
                    Fail(std::string("stream teardown failed: ") + e.what());
                } catch (...) {
                    Fail("stream teardown failed");
                }
                stream.reset();
                sdk_buffers.clear();
            }
            // No callback may retain a borrowed control object beyond device release.
            // Profile state has no transport ownership; destroy its captures while
            // the SDK connection and parameter objects are still alive.
            hooks = DeviceHooks{};
            if (device) {
                try {
                    if (device->IsConnected()) {
                        const auto result = device->Disconnect();
                        if (!result.IsOK()) {
                            Fail("device disconnect failed: " + common::Ebus::PvResultToString(result));
                        }
                    }
                } catch (const std::exception &e) {
                    Fail(std::string("device disconnect failed: ") + e.what());
                } catch (...) {
                    Fail("device disconnect failed");
                }
                device.reset();
            }
            if (recorder) {
                try {
                    if (recorder_open) {
                        if (recorder->RecordsWritten() != admitted.load() || unconfirmed.load() != 0) {
                            Fail("raw record count does not match admitted SDK buffers");
                        }
                        Event({
                            {"event", "capture_final"}, {"counters", Counters()}, {"reason", Reason()},
                            {"acquisition_started", acquisition_started}, {"failed", owner.HasFailed()}
                        });
                    }
                } catch (const std::exception &e) {
                    Fail(std::string("final capture metadata failed: ") + e.what());
                } catch (...) {
                    Fail("final capture metadata failed");
                }
                try {
                    recorder->Close(initialized && acquisition_started && !owner.HasFailed(), Reason());
                } catch (const std::exception &e) {
                    Fail(std::string("raw recorder close failed: ") + e.what());
                } catch (...) {
                    Fail("raw recorder close failed");
                }
            }
            shutdown_done = true;
            try {
                log.Info("[{}] camera shutdown completed; failed={}", options.id, owner.HasFailed());
            } catch (...) {
            }
        }
    };


    EbusCameraDriver::EbusCameraDriver(CameraDefinition definition, std::shared_ptr<common::SensorSyncHub> sync)
        : impl_(std::make_unique<Impl>(*this, std::move(definition), std::move(sync))) {
    }


    EbusCameraDriver::~EbusCameraDriver() {
        Shutdown();
    }


    bool EbusCameraDriver::Init(const StopPredicate &external_stop) {
        return impl_->Init(external_stop);
    }


    void EbusCameraDriver::Run() {
        impl_->Run();
    }


    void EbusCameraDriver::Shutdown() {
        impl_->Shutdown();
    }


    std::filesystem::path EbusCameraDriver::Directory() const {
        return impl_->options.recording.directory;
    }


    std::optional<std::uint64_t> EbusCameraDriver::MicrosSinceLastData() const {
        if (!impl_->options.expect_data || !impl_->acquiring.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        // All participants are prepared in Init, but their Run threads arm at
        // different instants. The hub's own 30-second guard owns that waiting time.
        if (impl_->registered && !impl_->sync_running_seen.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        const auto reference = impl_->data_reference_ns.load(std::memory_order_acquire);
        if (!reference) {
            return std::nullopt;
        }
        const auto now = common::TimeUtil::MonotonicNowNs();
        return now > reference ? (now - reference) / 1000 : 0;
    }


    nlohmann::ordered_json EbusCameraDriver::FinalStatistics() const {
        return {
            {"schema_version", 1}, {"driver", "unified-ebus"}, {"kind", impl_->options.kind},
            {"camera_id", impl_->options.id}, {"initialized", impl_->initialized},
            {"acquisition_started", impl_->acquisition_started}, {"shutdown_complete", impl_->shutdown_done},
            {"clean", impl_->shutdown_done && impl_->initialized && impl_->acquisition_started && !HasFailed()},
            {"failed", HasFailed()}, {"reason", impl_->Reason()},
            {"error", HasFailed() ? impl_->Reason() : std::string()}, {"counters", impl_->Counters()},
            {"frames_written", impl_->recorder ? impl_->recorder->FramesWritten() : 0},
            {"records_written", impl_->recorder ? impl_->recorder->RecordsWritten() : 0},
            {"trigger_counter", impl_->trigger_accounting}, {"sensor_sync", impl_->timing},
            {"recording", impl_->recorder ? impl_->recorder->Summary() : Json(nullptr)}
        };
    }
} // namespace ebus_driver
