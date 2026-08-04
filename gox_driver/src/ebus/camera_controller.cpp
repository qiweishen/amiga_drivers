#include "ebus/camera_controller.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "logger.h"
#include "ebus/sdk_error.hpp"

namespace jai::ebus {
    namespace {
        Common::DriverLog g_log{"GoX"};


        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        }


        // Comma-separated list of the currently selectable entries of an enum
        // feature — appended to SetValue failures so a wrong PixelFormat (etc.) in
        // config.json reports what the camera actually accepts.
        std::string available_enum_entries(PvGenEnum *ge) {
            int64_t count = 0;
            if (ge == nullptr || !ge->GetEntriesCount(count).IsOK()) {
                return {};
            }
            std::string out;
            for (int64_t i = 0; i < count; ++i) {
                const PvGenEnumEntry *entry = nullptr;
                if (!ge->GetEntryByIndex(i, &entry).IsOK() || entry == nullptr) {
                    continue;
                }
                bool available = false;
                if (!entry->IsAvailable(available).IsOK() || !available) {
                    continue;
                }
                PvString name;
                if (!entry->GetName(name).IsOK()) {
                    continue;
                }
                if (!out.empty()) {
                    out += ", ";
                }
                out += name.GetAscii();
            }
            return out;
        }


        bool parse_int64(const std::string &s, int64_t &out) {
            if (s.empty()) {
                return false;
            }
            errno = 0;
            char *end = nullptr;
            long long v = std::strtoll(s.c_str(), &end, 0);
            if (errno != 0 || end == s.c_str() || *end != '\0') {
                return false;
            }
            out = static_cast<int64_t>(v);
            return true;
        }


        bool parse_double(const std::string &s, double &out) {
            if (s.empty()) {
                return false;
            }
            errno = 0;
            char *end = nullptr;
            double v = std::strtod(s.c_str(), &end);
            if (errno != 0 || end == s.c_str() || *end != '\0') {
                return false;
            }
            out = v;
            return true;
        }


        bool parse_bool(const std::string &s, bool &out) {
            const std::string t = lower(s);
            if (t == "true" || t == "1" || t == "on") {
                out = true;
                return true;
            }
            if (t == "false" || t == "0" || t == "off") {
                out = false;
                return true;
            }
            return false;
        }


        std::string fmt_double(double v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.10g", v);
            return buf;
        }
    } // namespace


    bool apply_genicam_feature(PvGenParameterArray *params, const RawFeature &f, const std::string &context,
                               bool required, std::string *readback) {
        std::string rb_text;

        // fx10 semantics: a failed write (or a hard read-back mismatch) throws;
        // `required = false` degrades every failure to a WARN + false so
        // optional convenience features can be skipped.
        auto finish_fail = [&](const std::string &why) -> bool {
            const std::string msg = context + ": \"" + f.name + "\" = \"" + f.value + "\": " + why;
            if (required) {
                throw SdkError(msg);
            }
            g_log.warn("{}", msg);
            return false;
        };
        auto finish_ok = [&]() -> bool {
            if (readback != nullptr) {
                *readback = rb_text;
            }
            return true;
        };

        if (params == nullptr) {
            return finish_fail("no parameter array");
        }
        PvGenParameter *p = params->Get(PvString(f.name.c_str()));
        if (p == nullptr) {
            return finish_fail("parameter not found");
        }
        PvGenType type = PvGenTypeUndefined;
        PvResult tr = p->GetType(type);
        if (!tr.IsOK()) {
            return finish_fail("GetType: " + pv_result_to_string(tr));
        }

        if (type == PvGenTypeCommand) {
            auto *cmd = dynamic_cast<PvGenCommand *>(p);
            if (cmd == nullptr) {
                return finish_fail("not a command node");
            }
            PvResult r = cmd->Execute();
            if (!r.IsOK()) {
                return finish_fail("Execute: " + pv_result_to_string(r));
            }
            rb_text = "<executed>";
            return finish_ok();
        }

        if (!p->IsWritable()) {
            return finish_fail("parameter not writable");
        }

        switch (type) {
            case PvGenTypeInteger: {
                int64_t v = 0;
                if (!parse_int64(f.value, v)) {
                    return finish_fail("not a valid integer");
                }
                auto *gi = dynamic_cast<PvGenInteger *>(p);
                if (gi == nullptr) {
                    return finish_fail("type mismatch (integer)");
                }
                PvResult r = gi->SetValue(v);
                if (!r.IsOK()) {
                    return finish_fail("SetValue: " + pv_result_to_string(r));
                }
                int64_t rb = 0;
                r = gi->GetValue(rb);
                if (!r.IsOK()) {
                    return finish_fail("readback: " + pv_result_to_string(r));
                }
                rb_text = std::to_string(rb);
                if (rb != v) {
                    // fx10 setInt: the camera's rounding/clamping is its business
                    g_log.warn("{}: \"{}\" = {} requested, camera clamped to {}", context, f.name, v, rb);
                }
                return finish_ok();
            }
            case PvGenTypeFloat: {
                double v = 0;
                if (!parse_double(f.value, v)) {
                    return finish_fail("not a valid number");
                }
                auto *gf = dynamic_cast<PvGenFloat *>(p);
                if (gf == nullptr) {
                    return finish_fail("type mismatch (float)");
                }
                PvResult r = gf->SetValue(v);
                if (!r.IsOK()) {
                    return finish_fail("SetValue: " + pv_result_to_string(r));
                }
                double rb = 0;
                r = gf->GetValue(rb);
                if (!r.IsOK()) {
                    return finish_fail("readback: " + pv_result_to_string(r));
                }
                rb_text = fmt_double(rb);
                const double tolerance = std::max(std::fabs(v) * 1e-3, 1e-9);
                if (std::fabs(rb - v) > tolerance) {
                    // fx10 setFloat: warn on clamp instead of failing
                    g_log.warn("{}: \"{}\" = {} requested, camera clamped to {}", context, f.name, fmt_double(v),
                               rb_text);
                }
                return finish_ok();
            }
            case PvGenTypeBoolean: {
                bool v = false;
                if (!parse_bool(f.value, v)) {
                    return finish_fail("not a valid boolean (true/false/1/0/on/off)");
                }
                auto *gb = dynamic_cast<PvGenBoolean *>(p);
                if (gb == nullptr) {
                    return finish_fail("type mismatch (boolean)");
                }
                PvResult r = gb->SetValue(v);
                if (!r.IsOK()) {
                    return finish_fail("SetValue: " + pv_result_to_string(r));
                }
                bool rb = false;
                r = gb->GetValue(rb);
                if (!r.IsOK()) {
                    return finish_fail("readback: " + pv_result_to_string(r));
                }
                rb_text = rb ? "true" : "false";
                if (rb != v) {
                    return finish_fail("read-back mismatch after boolean write");
                }
                return finish_ok();
            }
            case PvGenTypeEnum: {
                auto *ge = dynamic_cast<PvGenEnum *>(p);
                if (ge == nullptr) {
                    return finish_fail("type mismatch (enum)");
                }
                int64_t iv = 0;
                const bool numeric = !f.value_is_string && parse_int64(f.value, iv);
                PvResult r = numeric ? ge->SetValue(iv) : ge->SetValue(PvString(f.value.c_str()));
                if (!r.IsOK()) {
                    std::string msg = "SetValue: " + pv_result_to_string(r);
                    const std::string entries = available_enum_entries(ge);
                    if (!entries.empty()) {
                        msg += " -- valid entries on this camera: [" + entries + "]";
                    }
                    return finish_fail(msg);
                }
                if (numeric) {
                    int64_t rb = 0;
                    r = ge->GetValue(rb);
                    if (!r.IsOK()) {
                        return finish_fail("readback: " + pv_result_to_string(r));
                    }
                    rb_text = std::to_string(rb);
                    if (rb != iv) {
                        return finish_fail(
                            "read-back mismatch: wrote " + std::to_string(iv) + ", camera reports " + rb_text);
                    }
                } else {
                    PvString rb;
                    r = ge->GetValue(rb);
                    if (!r.IsOK()) {
                        return finish_fail("readback: " + pv_result_to_string(r));
                    }
                    rb_text = to_std(rb);
                    if (rb_text != f.value) {
                        return finish_fail(
                            "read-back mismatch: wrote '" + f.value + "', camera reports '" + rb_text + "'");
                    }
                }
                return finish_ok();
            }
            case PvGenTypeString: {
                auto *gs = dynamic_cast<PvGenString *>(p);
                if (gs == nullptr) {
                    return finish_fail("type mismatch (string)");
                }
                PvResult r = gs->SetValue(PvString(f.value.c_str()));
                if (!r.IsOK()) {
                    return finish_fail("SetValue: " + pv_result_to_string(r));
                }
                PvString rb;
                r = gs->GetValue(rb);
                if (!r.IsOK()) {
                    return finish_fail("readback: " + pv_result_to_string(r));
                }
                rb_text = to_std(rb);
                if (rb_text != f.value) {
                    return finish_fail(
                        "read-back mismatch: wrote '" + f.value + "', camera reports '" + rb_text + "'");
                }
                return finish_ok();
            }
            default:
                return finish_fail("unsupported parameter type");
        }
    }

    bool feature_exists(PvGenParameterArray *params, const std::string &name) {
        return params != nullptr && params->Get(PvString(name.c_str())) != nullptr;
    }


    bool read_int_feature(PvGenParameterArray *params, const std::string &name, int64_t &out) {
        return params != nullptr && params->GetIntegerValue(PvString(name.c_str()), out).IsOK();
    }


    bool read_float_feature(PvGenParameterArray *params, const std::string &name, double &out) {
        return params != nullptr && params->GetFloatValue(PvString(name.c_str()), out).IsOK();
    }


    bool read_enum_feature(PvGenParameterArray *params, const std::string &name, std::string &out) {
        if (params == nullptr) {
            return false;
        }
        PvString v;
        if (!params->GetEnumValue(PvString(name.c_str()), v).IsOK()) {
            return false;
        }
        out = to_std(v);
        return true;
    }


    bool read_feature_as_string(PvGenParameterArray *params, const std::string &name, std::string &out) {
        if (params == nullptr) {
            return false;
        }
        PvGenParameter *p = params->Get(PvString(name.c_str()));
        if (p == nullptr || !p->IsReadable()) {
            return false;
        }
        PvString v;
        if (!p->ToString(v).IsOK()) {
            return false;
        }
        out = to_std(v);
        return true;
    }


    bool execute_command_feature(PvGenParameterArray *params, const std::string &name) {
        if (params == nullptr) {
            return false;
        }
        auto *cmd = dynamic_cast<PvGenCommand *>(params->Get(PvString(name.c_str())));
        return cmd != nullptr && cmd->Execute().IsOK();
    }


    CameraController::CameraController(std::string camera_id, StopController *stop)
        : camera_id_(std::move(camera_id)), stop_(stop) {
    }


    CameraController::~CameraController() {
        try {
            disconnect();
        } catch (const std::exception &e) {
            g_log.warn("[{}] Disconnect during teardown failed: {}", camera_id_, e.what());
        }
    }


    void CameraController::connect(const std::string &target, DiscoveredDevice identity) {
        identity_ = std::move(identity);
        device_ = std::make_unique<PvDeviceGEV>();
        g_log.info("[{}] Connecting to {} ...", camera_id_, target);
        CHECK_PV(device_->Connect(PvString(target.c_str()), PvAccessControl), "PvDeviceGEV::Connect");

        // Direct dials skip discovery
        if (identity_.model.empty()) {
            read_feature_as_string(params(), "DeviceModelName", identity_.model);
        }
        if (identity_.serial.empty()) {
            read_feature_as_string(params(), "DeviceSerialNumber", identity_.serial);
        }
        if (identity_.vendor.empty()) {
            read_feature_as_string(params(), "DeviceVendorName", identity_.vendor);
        }
        if (identity_.firmware.empty() && !read_feature_as_string(params(), "DeviceVersion", identity_.firmware)) {
            read_feature_as_string(params(), "DeviceFirmwareVersion", identity_.firmware);
        }
        if (identity_.user_name.empty()) {
            read_feature_as_string(params(), "DeviceUserID", identity_.user_name);
        }

        PvResult r = device_->RegisterEventSink(this);
        if (!r.IsOK()) {
            g_log.warn("[{}] [eBUS] RegisterEventSink failed (link-loss detection degraded): {}", camera_id_,
                       pv_result_to_string(r));
        }
    }


    void CameraController::disconnect() {
        if (!device_) {
            return;
        }
        device_->UnregisterEventSink(this);
        if (device_->IsConnected()) {
            device_->Disconnect();
        }
        device_.reset();
    }


    bool CameraController::connected() const {
        return device_ && device_->IsConnected();
    }


    PvGenParameterArray *CameraController::params() {
        return device_ ? device_->GetParameters() : nullptr;
    }


    uint32_t CameraController::payload_size() {
        if (!device_) {
            throw SdkError("payload_size: device not connected");
        }
        return device_->GetPayloadSize();
    }


    bool CameraController::try_apply(const std::string &name, const std::string &value, bool value_is_string,
                                     bool required) {
        if (!required && !feature_exists(params(), name)) {
            g_log.warn("[{}] [eBUS] Optional feature {} not present; skipped", camera_id_, name);
            return false;
        }
        RawFeature f;
        f.name = name;
        f.value = value;
        f.value_is_string = value_is_string;
        std::string readback;
        const bool ok = apply_genicam_feature(params(), f, "[" + camera_id_ + "] device", required, &readback);
        if (ok) {
            g_log.trace("[{}] [eBUS] {} = {}{}", camera_id_, name, value,
                        readback.empty() ? "" : " (readback " + readback + ")");
        }
        return ok;
    }

    void CameraController::apply_config(const CameraConfig &cfg) {
        const std::string context = "[" + camera_id_ + "] device";

        // 1. Disable automatics first so the manual exposure/gain writes stick.
        try_apply("ExposureAuto", "Off", true, false);
        try_apply("GainAuto", "Off", true, false);

        // 2. Binning entries from features.raw are hoisted here because
        // binning changes the Width/Height limits, so it must precede the ROI
        // writes. Relative order is preserved; the entries are skipped in step 9.
        std::vector<bool> hoisted(cfg.features.raw.size(), false);
        for (size_t i = 0; i < cfg.features.raw.size(); ++i) {
            if (cfg.features.raw[i].name.rfind("Binning", 0) == 0) {
                hoisted[i] = true;
                g_log.trace("[{}] [eBUS] Applying {} before ROI (binning changes size limits)", camera_id_,
                            cfg.features.raw[i].name);
                apply_genicam_feature(params(), cfg.features.raw[i], context);
            }
        }

        // 3. ROI in the safe order: zero offsets, then size, then final offsets.
        if (cfg.acquisition.roi) {
            const RoiConfig &roi = *cfg.acquisition.roi;
            if (roi.width != 0 || roi.height != 0) {
                try_apply("OffsetX", "0", false, false);
                try_apply("OffsetY", "0", false, false);
                if (roi.width != 0) {
                    try_apply("Width", std::to_string(roi.width), false, true);
                }
                if (roi.height != 0) {
                    try_apply("Height", std::to_string(roi.height), false, true);
                }
            }
            if (roi.width != 0 || roi.height != 0 || roi.offset_x != 0 || roi.offset_y != 0) {
                try_apply("OffsetX", std::to_string(roi.offset_x), false, false);
                try_apply("OffsetY", std::to_string(roi.offset_y), false, false);
            }
        }

        // 4. PixelFormat (before anything payload-size dependent).
        if (cfg.acquisition.pixel_format) {
            try_apply("PixelFormat", *cfg.acquisition.pixel_format, true, true);
        }

        // 5. Exposure: ExposureTime with ExposureTimeAbs fallback (older SFNC).
        // The config value is ms (fx10 convention); the GenICam node is µs.
        if (cfg.acquisition.exposure_ms) {
            std::string name;
            if (feature_exists(params(), "ExposureTime")) {
                name = "ExposureTime";
            } else if (feature_exists(params(), "ExposureTimeAbs")) {
                name = "ExposureTimeAbs";
            }
            if (name.empty()) {
                g_log.warn("[{}] [eBUS] No ExposureTime/ExposureTimeAbs feature; exposure_ms not applied", camera_id_);
            } else {
                try_apply(name, fmt_double(*cfg.acquisition.exposure_ms * 1000.0), false, true);
            }
        }

        // 6. Gain: GainSelector=AnalogAll, fallback All, fallback no selector.
        if (cfg.acquisition.gain) {
            if (feature_exists(params(), "GainSelector")) {
                if (!try_apply("GainSelector", "AnalogAll", true, false)) {
                    if (!try_apply("GainSelector", "All", true, false)) {
                        g_log.trace(
                            "[{}] [eBUS] GainSelector accepts neither AnalogAll nor All; using current selector position",
                            camera_id_);
                    }
                }
            }
            try_apply("Gain", fmt_double(*cfg.acquisition.gain), false, true);
        }

        // 7. Frame rate (freerun only — under external trigger the pulse train
        // drives frame timing, same as fx10).
        const TriggerConfig &t = cfg.acquisition.trigger;
        const bool external = t.mode == "external";
        if (cfg.acquisition.frame_rate_hz) {
            if (external) {
                g_log.trace("[{}] [eBUS] External trigger; frame_rate_hz ignored (frame timing follows the trigger)",
                            camera_id_);
            } else {
                try_apply("AcquisitionFrameRateEnable", "true", false, false);
                std::string name;
                if (feature_exists(params(), "AcquisitionFrameRate")) {
                    name = "AcquisitionFrameRate";
                } else if (feature_exists(params(), "AcquisitionFrameRateAbs")) {
                    name = "AcquisitionFrameRateAbs";
                }
                if (name.empty()) {
                    g_log.warn("[{}] [eBUS] No AcquisitionFrameRate feature; frame_rate_hz not applied", camera_id_);
                } else {
                    try_apply(name, fmt_double(*cfg.acquisition.frame_rate_hz), false, true);
                }
            }
        }

        // 8. Trigger (fx10 shape): selector first so TriggerMode targets the
        // right trigger; freerun always writes Off (clears a sticky mode left
        // by a previous run).
        try_apply("TriggerSelector", t.selector_entry, true, false);
        if (external) {
            try_apply("TriggerSource", t.source_entry, true, true);
            try_apply("TriggerActivation", t.activation == "rising" ? "RisingEdge" : "FallingEdge",
                      true, false);
            try_apply("TriggerMode", "On", true, true);
        } else {
            try_apply("TriggerMode", "Off", true, true);
        }

        // 9. User feature list, strictly in order (binning already applied).
        for (size_t i = 0; i < cfg.features.raw.size(); ++i) {
            if (hoisted[i]) {
                continue;
            }
            apply_genicam_feature(params(), cfg.features.raw[i], context);
        }

        // 10. 64-bit BlockIDs: 16-bit IDs wrap every 65536 frames and break the
        // gap accounting, so force extended mode on when the camera has it.
        std::string ext_mode;
        if (read_enum_feature(params(), "GevGVSPExtendedIDMode", ext_mode)) {
            if (lower(ext_mode) == "off") {
                g_log.warn("[{}] [eBUS] GevGVSPExtendedIDMode is Off; Switching to On (64-bit BlockIDs)", camera_id_);
                try_apply("GevGVSPExtendedIDMode", "On", true, false);
            }
        } else {
            g_log.trace("[{}] [eBUS] GevGVSPExtendedIDMode not present", camera_id_);
        }
    }


    void CameraController::stream_enable() {
        if (!device_) {
            throw SdkError("stream_enable: device not connected");
        }
        CHECK_PV(device_->StreamEnable(), "PvDevice::StreamEnable");
    }

    void CameraController::stream_disable(bool ignore_errors) {
        if (!device_) {
            return;
        }
        PvResult r = device_->StreamDisable();
        if (!r.IsOK()) {
            if (!ignore_errors && !link_lost()) {
                throw SdkError("PvDevice::StreamDisable", r);
            }
            g_log.trace("[{}] [eBUS] StreamDisable ignored failure: {}", camera_id_, pv_result_to_string(r));
        }
    }


    void CameraController::acquisition_start() {
        auto *cmd = dynamic_cast<PvGenCommand *>(params()->Get(PvString("AcquisitionStart")));
        if (cmd == nullptr) {
            throw SdkError("AcquisitionStart command not found on device");
        }
        CHECK_PV(cmd->Execute(), "AcquisitionStart");
        g_log.info("[{}] AcquisitionStart executed", camera_id_);
    }


    void CameraController::acquisition_stop(bool ignore_errors) {
        if (!device_ || !device_->IsConnected()) {
            return;
        }
        auto *cmd = dynamic_cast<PvGenCommand *>(params()->Get(PvString("AcquisitionStop")));
        if (cmd == nullptr) {
            if (ignore_errors) {
                return;
            }
            throw SdkError("AcquisitionStop command not found on device");
        }
        PvResult r = cmd->Execute();
        if (!r.IsOK()) {
            if (!ignore_errors && !link_lost()) {
                throw SdkError("AcquisitionStop", r);
            }
            g_log.debug("[{}] AcquisitionStop ignored failure: {}", camera_id_, pv_result_to_string(r));
        } else {
            g_log.info("[{}] AcquisitionStop executed", camera_id_);
        }
    }


    void CameraController::OnLinkDisconnected(PvDevice *) {
        link_lost_.store(true, std::memory_order_relaxed);
        g_log.error(
            "[{}] device link lost; stopping to preserve captured data (automatic reconnect is not implemented in v1)",
            camera_id_);
        if (stop_ != nullptr) {
            stop_->request_stop(StopReason::Error);
        }
    }
} // namespace jai::ebus
