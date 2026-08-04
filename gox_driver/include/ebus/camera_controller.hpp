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
#include <string>

#include "app_config.hpp"
#include "signal_stop.hpp"
#include "ebus/discovery.hpp"

namespace jai::ebus {
    // Generic typed GenICam write with fx10's verify-set semantics. Dispatches
    // on PvGenParameter::GetType() (Integer/Float/Boolean/Enum/String/Command)
    // and always reads the value back: Integer/Float clamping by the camera is
    // the camera's business (WARN, fx10 setInt/setFloat), Boolean/Enum/String
    // mismatches are failures, Commands are executed and never verified. A
    // failure throws SdkError when `required`, otherwise it degrades to a WARN
    // and returns false. `readback`, when non-null, receives the read-back
    // value on success (for logging).
    bool apply_genicam_feature(PvGenParameterArray *params, const RawFeature &f, const std::string &context,
                               bool required = true, std::string *readback = nullptr);

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
        explicit CameraController(std::string camera_id, StopController *stop);

        ~CameraController();

        CameraController(const CameraController &) = delete;

        CameraController &operator=(const CameraController &) = delete;

        // Connects by connection string (IP address or a discovery connection
        // ID — no PvSystem needed, fx10-aligned) with PvAccessControl (not
        // Exclusive: keeps eBUS Player read-only debugging possible), sets
        // communication parameters (AnswerTimeout/CommandRetries) and registers
        // the link-disconnected sink. `identity` carries what the caller knows
        // (full from discovery, just the IP on a direct dial); missing fields
        // are filled from the standard GenICam device nodes after connect.
        void connect(const std::string &target, DiscoveredDevice identity);

        // Unregisters the sink and disconnects. Idempotent.
        void disconnect();

        bool connected() const;

        bool link_lost() const { return link_lost_.load(std::memory_order_relaxed); }

        // Ordered configuration per plan: autos off -> binning (hoisted from
        // features.raw) -> offsets zeroed -> Width -> Height -> OffsetX/Y ->
        // PixelFormat -> exposure/gain/frame_rate/trigger acquisition mappings
        // -> remaining features.raw in listed order -> GevGVSPExtendedIDMode
        // forced On when Off. Throws SdkError when a feature with policy
        // "fail" cannot be applied.
        void apply_config(const CameraConfig &cfg);

        // TLParamsLocked management + acquisition commands (PvGenCommand).
        void stream_enable();

        void stream_disable(bool ignore_errors);

        void acquisition_start();

        void acquisition_stop(bool ignore_errors);

        uint32_t payload_size();

        PvGenParameterArray *params();

        PvDeviceGEV *device() { return device_.get(); }
        const DiscoveredDevice &identity() const { return identity_; }

    protected:
        // PvDeviceEventSink
        void OnLinkDisconnected(PvDevice *device) override;

    private:
        // Applies name=value. When required is false a missing parameter (or
        // any failure) is a WARN-level no-op returning false.
        bool try_apply(const std::string &name, const std::string &value, bool value_is_string, bool required);

        std::string camera_id_;
        StopController *stop_;

        std::unique_ptr<PvDeviceGEV> device_;
        DiscoveredDevice identity_;
        std::atomic<bool> link_lost_{false};
    };
} // namespace jai::ebus
