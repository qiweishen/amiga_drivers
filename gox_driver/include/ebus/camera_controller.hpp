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
    // Generic typed GenICam write. Dispatches on PvGenParameter::GetType()
    // (Integer/Float/Boolean/Enum/String/Command) and, when
    // apply.verify_readback is set, reads the value back and verifies it:
    // Integer within GetIncrement() rounding, Float within
    // float_verify_tolerance_rel (relative), everything else exact. Commands are
    // executed, never verified. On failure the effective on_error policy
    // (f.on_error, falling back to apply.on_error_default) decides: "fail"
    // throws SdkError, "warn" logs a warning, "skip" logs at debug level.
    // Returns true when the value was applied (and verified). `readback`, when
    // non-null, receives the read-back value on success (for logging).
    bool apply_genicam_feature(PvGenParameterArray *params, const GenicamFeature &f, const ApplyConfig &apply,
                               const std::string &context, std::string *readback = nullptr);

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
        // genicam_features) -> offsets zeroed -> Width -> Height -> OffsetX/Y ->
        // PixelFormat -> exposure/gain/frame_rate/trigger convenience mappings ->
        // remaining genicam_features in listed order -> GevGVSPExtendedIDMode
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
        // Applies name=value with an explicit policy. When required is false a
        // missing parameter is a silent (debug-level) no-op returning false.
        bool try_apply(const std::string &name, const std::string &value, bool value_is_string,
                       const ApplyConfig &apply,
                       const std::string &policy, bool required);

        std::string camera_id_;
        StopController *stop_;

        std::unique_ptr<PvDeviceGEV> device_;
        DiscoveredDevice identity_;
        std::atomic<bool> link_lost_{false};
    };
} // namespace jai::ebus
