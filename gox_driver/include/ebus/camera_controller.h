#pragma once

// Device control plane: owns the PvDeviceGEV, walks the ordered GenICam write
// plan (apply_plan.hpp) with write-then-read-back verification, and wraps
// StreamEnable/AcquisitionStart/Stop. The link-disconnected event sink feeds
// straight into the process-wide StopController (v1: no reconnect).

#include <PvDevice.h>
#include <PvDeviceGEV.h>
#include <PvGenParameter.h>
#include <PvGenParameterArray.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app_config.h"
#include "apply_plan.h"
#include "device_json.h"
#include "signal_stop.h"
#include "ebus/discovery.h" // common/ (amiga_ebus)
#include "ebus/sdk_error.h" // common/ (amiga_ebus)

namespace gox::ebus {
    // The SDK plumbing shared with fx10 lives in common/ (namespace
    // common::Ebus); these names are used throughout the gox eBUS layer.
    using common::Ebus::DiscoveredDevice;
    using common::Ebus::SdkError;
    using common::Ebus::PvResultToString;
    using common::Ebus::ToStd;

    // Typed GenICam write by node type, always read back. Bool/Enum/String mismatches fail;
    // Integer/Float clamps fail only when `strict`. Throws SdkError when `required`, otherwise
    // warns and returns false. `readback` / `tolerance_rel` (float nodes) feed device.json.
    bool apply_genicam_feature(PvGenParameterArray *params, const FeatureWrite &f, const std::string &context,
                               bool required = true, std::string *readback = nullptr,
                               double *tolerance_rel = nullptr);

    // By-name helpers; false (silently) when the parameter is missing or the access fails
    bool FeatureExists(PvGenParameterArray *params, const std::string &name);

    bool ReadIntFeature(PvGenParameterArray *params, const std::string &name, int64_t &out);

    bool ReadEnumFeature(PvGenParameterArray *params, const std::string &name, std::string &out);

    bool ReadFeatureAsString(PvGenParameterArray *params, const std::string &name, std::string &out);

    bool ReadFloatFeature(PvGenParameterArray *params, const std::string &name, double &out);

    // PvGenFloat::GetMin/GetMax, recorded in device.json next to the value
    bool ReadFloatBounds(PvGenParameterArray *params, const std::string &name, double &min_out, double &max_out);

    // Enum entry as the integer value the manual prints (stable across firmware, unlike the name)
    bool ReadEnumIntFeature(PvGenParameterArray *params, const std::string &name, int64_t &out);

    // Selector write without read-back (selectors are indices, restored afterwards)
    bool WriteEnumFeature(PvGenParameterArray *params, const std::string &name, int64_t value);

    bool ExecuteCommandFeature(PvGenParameterArray *params, const std::string &name);

    class CameraController : protected PvDeviceEventSink {
    public:
        explicit CameraController(std::string camera_id, StopController *stop);

        ~CameraController();

        CameraController(const CameraController &) = delete;

        CameraController &operator=(const CameraController &) = delete;

        // Connect by IP or discovery connection ID with PvAccessControl (eBUS Player can still
        // read); `identity` is completed from the device nodes after connect
        void Connect(const std::string &target, DiscoveredDevice identity);

        // UserSetSelector=Default + UserSetLoad, waited until complete (docs/DEVICE_CONFIG.md).
        // Before PTP enable and the stream open (the load resets GevSCPSPacketSize/GevSCPD)
        FactoryLoadResult LoadFactoryDefaults();

        // Unregisters the sink and disconnects. Idempotent.
        void Disconnect();

        bool Connected() const;

        bool LinkLost() const { return link_lost_.load(std::memory_order_relaxed); }

        // Walks BuildApplyPlan(cfg); throws SdkError on the first required write that cannot be
        // applied or verified. Returns device.json's "applied" block in plan order
        std::vector<AppliedFeature> ApplyConfig(const CameraConfig &cfg);

        // Clears Counter0 so trig= counts from AcquisitionStart (manual p.116); false = no counter
        bool ResetTriggerCounter();

        // Everything device.json needs from the connected device; never throws (unreadable
        // features are recorded as such)
        DeviceReport CollectDeviceReport(const CameraConfig &cfg, std::vector<AppliedFeature> applied,
                                           const PtpSummary &ptp, const FactoryLoadResult &factory_load,
                                           const RuntimeShape &runtime);

        // One telemetry row: temperatures, Counter0 (when bound), PAUSE frames; PTP is the caller's
        TelemetrySample SampleTelemetry(bool want_counter);

        // TLParamsLocked management + acquisition commands (PvGenCommand).
        void StreamEnable();

        void StreamDisable(bool ignore_errors);

        void AcquisitionStart();

        void AcquisitionStop(bool ignore_errors);

        uint32_t PayloadSize();

        PvGenParameterArray *Params();

        PvDeviceGEV *Device() { return device_.get(); }
        const DiscoveredDevice &Identity() const { return identity_; }

    protected:
        // PvDeviceEventSink
        void OnLinkDisconnected(PvDevice *device) override;

    private:
        std::string camera_id_;
        StopController *stop_;

        std::unique_ptr<PvDeviceGEV> device_;
        DiscoveredDevice identity_;
        // Telemetry capability probes, resolved on the first sample
        bool telemetry_probed_ = false;
        bool have_temperature_ = false;
        bool have_counter_ = false;
        bool have_pause_counter_ = false;
        bool temp_warned_ = false;
        std::atomic<bool> link_lost_{false};
    };
} // namespace gox::ebus
