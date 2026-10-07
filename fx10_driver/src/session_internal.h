#pragma once

// Private implementation boundary: SDK types and connection ownership never
// appear in the public Session interface or the IDriverApp adapter.
#include "session.h"
#include "app_config.h"
#include "envi_recorder.h"
#include "logger.h"
#include "sensor_sync_hub.h"
#include "session_metadata.h"
#include "ebus/camera_control.h"
#include "ebus/stream_receiver.h"

namespace fx10 {
namespace session_detail {
    // One logger and one set of session policies across all implementation units.
    extern common::DriverLog g_log;
    // No timing-log growth for this long = the Teensy USB link is down
    inline constexpr double kTriggerLogStallAbortS = 15.0;

    // The shared SensorSync session STARTs once every registered camera has armed.
    // Armed this long without a START = another participant never got there.
    inline constexpr double kSensorSyncStartWaitS = 30.0;

    // This driver's name in the rig-wide SensorSync session (common::SensorSyncHub)
    inline constexpr const char *kSensorSyncOwner = "fx10";

    // Device telemetry is a blocking GVCP round trip that shares the link with
    // the stream, so it gets a slow cadence of its own and is NEVER tied to
    // logging.stats_interval_s (which an operator may set to 0).
    inline constexpr double kDeviceTelemetryIntervalS = 60.0;

    // Manual Table 8 (p.44): the camera CANCELS operation above these internal
    // temperatures. Warn before that happens; re-arm after a 5 C drop so a
    // camera sitting near the line does not flood the log.
    inline constexpr double kProcPcbLimitC = 80.0;
    inline constexpr double kFpgaLimitC = 90.0;
    inline constexpr double kThermalWarnMarginC = 5.0;
    inline constexpr double kThermalRearmMarginC = 10.0;
} // namespace session_detail

// Connection resources are owned by the session thread. Final counters are
// sampled only after receiver and recorder workers stop.
struct Session::Impl {
    struct Connection {
        fx10::Counters counters;
        std::unique_ptr<fx10::EnviRecorder> recorder; // built in startStreaming_
        fx10::StreamReceiver receiver;
        std::unique_ptr<fx10::CameraControl> control; // built after connect, before the stream open
        fx10::RecorderInit recorder_init;
        fx10::CameraControl::Geometry geometry;
        std::uint32_t bytes_per_pixel = 2;
        std::int64_t missed_baseline = -1;
        fx10::JsonlFile telemetry;
        fx10::DeviceTelemetry latest_telemetry;
        std::optional<std::int64_t> previous_missed;
        bool counter_regressed = false;
        bool counter_binding_verified = false;
        bool metadata_failed = false;
        bool acquisition_started = false;

        // Called only by the session owner, outside the receive callback. The
        // periodic guard and logging consume this SAME sample (no extra reads).
        bool RecordTelemetry(const std::string &phase, bool establish_baseline = false);

        explicit Connection(const fx10::NetworkConfig &network) : receiver(network, counters) {
        }
    };

    fx10::AppConfig config;
    std::unique_ptr<Connection> session;

    std::string stop_reason = "completed"; // logged by EnviRecorder::Stop as the exit reason
    std::string last_error;
    int last_exit_code = 0; // standalone exit-code semantics, reported at shutdown
    bool run_incomplete = false; // checked only after Run joins
    nlohmann::ordered_json final_statistics = {{"schema_version", 1}, {"status", "not_started"},
        {"acquisition_started", false}, {"counters", fx10::FinalCountersJson(fx10::Counters{})}};

    // The rig's SensorSync board is shared with the Go-X driver: this is one
    // participant of the session main owns (registered in bring-up, armed after
    // AcquisitionStart, disarmed before the stream stops)
    std::shared_ptr<common::SensorSyncHub> sync;
    bool sync_participant = false;
    std::string observations_file; // timing log path relative to the ENVI session directory
};

} // namespace fx10
