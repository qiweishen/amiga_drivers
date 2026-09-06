#include "ebus/camera_controller.h"
#include "time_util.h"

#include <PvGenBoolean.h>
#include <PvGenCommand.h>
#include <PvGenEnum.h>
#include <PvGenInteger.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "logger.h"
#include "string_util.h"
#include "user_set.h"
#include "util.h"
#include "ebus/sdk_error.h"

namespace gox::ebus {
    namespace {
        common::DriverLog g_log{"GoX"};

        // UserSetLoad is a flash read: raise the GVCP answer timeout like the
        // SDK's own eBUS Player does (UserSetsManager.cpp), then wait on IsDone.
        constexpr int64_t kUserSetAnswerTimeoutMs = 10000;
        constexpr int kUserSetLoadDeadlineMs = 15000;
        constexpr int kUserSetPollMs = 50;


        // Names of the currently selectable entries of an enum feature.
        std::vector<std::string> EnumEntryNames(PvGenEnum *ge) {
            std::vector<std::string> out;
            int64_t count = 0;
            if (ge == nullptr || !ge->GetEntriesCount(count).IsOK()) {
                return out;
            }
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
                out.push_back(ToStd(name));
            }
            return out;
        }


        // Comma-separated list of the currently selectable entries of an enum
        // feature — appended to SetValue failures so a wrong PixelFormat (etc.) in
        // config.json reports what the camera actually accepts.
        std::string AvailableEnumEntries(PvGenEnum *ge) {
            std::string out;
            for (const auto &name: EnumEntryNames(ge)) {
                if (!out.empty()) {
                    out += ", ";
                }
                out += name;
            }
            return out;
        }


        bool ParameterUsable(PvGenParameter *p) {
            return p != nullptr && p->IsImplemented() && p->IsAvailable();
        }


        const char *DescribeParameter(PvGenParameter *p) {
            if (p == nullptr) {
                return "missing";
            }
            if (!p->IsImplemented()) {
                return "not implemented";
            }
            return p->IsAvailable() ? "ok" : "not available";
        }


        // Raises AnswerTimeout and suppresses disconnect-on-timeout for the
        // duration of a slow command; restores both on every exit path.
        class CommandTimeoutGuard {
        public:
            CommandTimeoutGuard(PvGenParameterArray *comm, std::string camera_id) : camera_id_(std::move(camera_id)) {
                if (comm == nullptr) {
                    throw SdkError("communication parameters unavailable");
                }
                answer_ = comm->GetInteger(PvString("AnswerTimeout"));
                if (answer_ == nullptr || !answer_->GetValue(old_answer_).IsOK()) {
                    throw SdkError("AnswerTimeout communication parameter unavailable");
                }
                CHECK_PV(answer_->SetValue(std::max(old_answer_, kUserSetAnswerTimeoutMs)), "AnswerTimeout");
                disconnect_ = comm->GetBoolean(PvString("DisconnectOnAnyTimeout"));
                if (disconnect_ != nullptr && disconnect_->GetValue(old_disconnect_).IsOK()) {
                    have_disconnect_ = disconnect_->SetValue(false).IsOK();
                }
            }

            ~CommandTimeoutGuard() {
                if (!answer_->SetValue(old_answer_).IsOK()) {
                    g_log.Warn("[{}] [eBUS] AnswerTimeout could not be restored to {} ms", camera_id_, old_answer_);
                }
                if (have_disconnect_ && !disconnect_->SetValue(old_disconnect_).IsOK()) {
                    g_log.Warn("[{}] [eBUS] DisconnectOnAnyTimeout could not be restored", camera_id_);
                }
            }

            CommandTimeoutGuard(const CommandTimeoutGuard &) = delete;
            CommandTimeoutGuard &operator=(const CommandTimeoutGuard &) = delete;

        private:
            std::string camera_id_;
            PvGenInteger *answer_ = nullptr;
            PvGenBoolean *disconnect_ = nullptr;
            int64_t old_answer_ = 0;
            bool old_disconnect_ = false;
            bool have_disconnect_ = false;
        };


        // Polls IsDone until the command reports completion; a camera without
        // command completion reporting is taken as done once Execute returned.
        // Returns whether IsDone was actually usable (recorded in device.json:
        // without it the factory load is only known to have been *sent*).
        bool WaitCommandDone(PvGenCommand *cmd, const char *name, const std::string &camera_id) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUserSetLoadDeadlineMs);
            for (;;) {
                bool done = false;
                const PvResult r = cmd->IsDone(done);
                if (!r.IsOK()) {
                    g_log.Trace("[{}] [eBUS] {} IsDone not supported ({}); assuming complete after Execute", camera_id,
                                name, PvResultToString(r));
                    return false;
                }
                if (done) {
                    return true;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw SdkError(std::string(name) + " did not complete within " +
                                   std::to_string(kUserSetLoadDeadlineMs) + " ms");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kUserSetPollMs));
            }
        }


        bool ParseInt64(const std::string &s, int64_t &out) {
            if (s.empty()) {
                return false;
            }
            errno = 0;
            char *end = nullptr;
            long long v = std::strtoll(s.c_str(), &end, 10);
            if (errno != 0 || end == s.c_str() || *end != '\0') {
                return false;
            }
            out = static_cast<int64_t>(v);
            return true;
        }


        bool ParseDouble(const std::string &s, double &out) {
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


        bool ParseBool(const std::string &s, bool &out) {
            const std::string t = common::StringUtil::ToLower(s);
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


        std::string FmtDouble(double v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.10g", v);
            return buf;
        }
    } // namespace


    bool apply_genicam_feature(PvGenParameterArray *params, const FeatureWrite &f, const std::string &context,
                               bool required, std::string *readback, double *tolerance_rel) {
        std::string rb_text;
        double rb_tolerance = 0.0;

        // A failed write (or a hard read-back mismatch) throws; `required =
        // false` degrades every failure to a WARN + false so optional
        // convenience features can be skipped.
        auto finish_fail = [&](const std::string &why) -> bool {
            const std::string msg = context + ": \"" + f.name + "\" = \"" + f.value + "\": " + why;
            if (required) {
                throw SdkError(msg);
            }
            g_log.Warn("{}", msg);
            return false;
        };
        auto finish_ok = [&]() -> bool {
            if (readback != nullptr) {
                *readback = rb_text;
            }
            if (tolerance_rel != nullptr) {
                *tolerance_rel = rb_tolerance;
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
            return finish_fail("GetType: " + PvResultToString(tr));
        }

        if (type == PvGenTypeCommand) {
            auto *cmd = dynamic_cast<PvGenCommand *>(p);
            if (cmd == nullptr) {
                return finish_fail("not a command node");
            }
            PvResult r = cmd->Execute();
            if (!r.IsOK()) {
                return finish_fail("Execute: " + PvResultToString(r));
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
                if (!ParseInt64(f.value, v)) {
                    return finish_fail("not a valid integer");
                }
                auto *gi = dynamic_cast<PvGenInteger *>(p);
                if (gi == nullptr) {
                    return finish_fail("type mismatch (integer)");
                }
                PvResult r = gi->SetValue(v);
                if (!r.IsOK()) {
                    return finish_fail("SetValue: " + PvResultToString(r));
                }
                int64_t rb = 0;
                r = gi->GetValue(rb);
                if (!r.IsOK()) {
                    return finish_fail("readback: " + PvResultToString(r));
                }
                rb_text = std::to_string(rb);
                if (rb != v) {
                    if (f.strict) {
                        return finish_fail("camera clamped the value to " + rb_text +
                                           "; the requested value is not achievable with the current settings");
                    }
                    // features.raw: the camera's rounding/clamping is its business
                    g_log.Warn("{}: \"{}\" = {} requested, camera clamped to {}", context, f.name, v, rb);
                }
                return finish_ok();
            }
            case PvGenTypeFloat: {
                double v = 0;
                if (!ParseDouble(f.value, v)) {
                    return finish_fail("not a valid number");
                }
                auto *gf = dynamic_cast<PvGenFloat *>(p);
                if (gf == nullptr) {
                    return finish_fail("type mismatch (float)");
                }
                PvResult r = gf->SetValue(v);
                if (!r.IsOK()) {
                    return finish_fail("SetValue: " + PvResultToString(r));
                }
                double rb = 0;
                r = gf->GetValue(rb);
                if (!r.IsOK()) {
                    return finish_fail("readback: " + PvResultToString(r));
                }
                rb_text = FmtDouble(rb);
                // Strict writes allow 1 % for the node's own step quantisation
                // (the frame rate is not continuous); features.raw keeps fx10's
                // tighter warn-only threshold.
                rb_tolerance = f.strict ? 1e-2 : 1e-3;
                const double tolerance = std::max(std::fabs(v) * rb_tolerance, 1e-9);
                if (std::fabs(rb - v) > tolerance) {
                    if (f.strict) {
                        return finish_fail("camera clamped the value to " + rb_text +
                                           "; the requested value is not achievable with the current settings");
                    }
                    g_log.Warn("{}: \"{}\" = {} requested, camera clamped to {}", context, f.name, FmtDouble(v),
                               rb_text);
                }
                return finish_ok();
            }
            case PvGenTypeBoolean: {
                bool v = false;
                if (!ParseBool(f.value, v)) {
                    return finish_fail("not a valid boolean (true/false/1/0/on/off)");
                }
                auto *gb = dynamic_cast<PvGenBoolean *>(p);
                if (gb == nullptr) {
                    return finish_fail("type mismatch (boolean)");
                }
                PvResult r = gb->SetValue(v);
                if (!r.IsOK()) {
                    return finish_fail("SetValue: " + PvResultToString(r));
                }
                bool rb = false;
                r = gb->GetValue(rb);
                if (!r.IsOK()) {
                    return finish_fail("readback: " + PvResultToString(r));
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
                const bool numeric = !f.value_is_string && ParseInt64(f.value, iv);
                PvResult r = numeric ? ge->SetValue(iv) : ge->SetValue(PvString(f.value.c_str()));
                if (!r.IsOK()) {
                    std::string msg = "SetValue: " + PvResultToString(r);
                    const std::string entries = AvailableEnumEntries(ge);
                    if (!entries.empty()) {
                        msg += " -- valid entries on this camera: [" + entries + "]";
                    }
                    return finish_fail(msg);
                }
                if (numeric) {
                    int64_t rb = 0;
                    r = ge->GetValue(rb);
                    if (!r.IsOK()) {
                        return finish_fail("readback: " + PvResultToString(r));
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
                        return finish_fail("readback: " + PvResultToString(r));
                    }
                    rb_text = ToStd(rb);
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
                    return finish_fail("SetValue: " + PvResultToString(r));
                }
                PvString rb;
                r = gs->GetValue(rb);
                if (!r.IsOK()) {
                    return finish_fail("readback: " + PvResultToString(r));
                }
                rb_text = ToStd(rb);
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

    bool FeatureExists(PvGenParameterArray *params, const std::string &name) {
        return params != nullptr && params->Get(PvString(name.c_str())) != nullptr;
    }


    bool ReadIntFeature(PvGenParameterArray *params, const std::string &name, int64_t &out) {
        return params != nullptr && params->GetIntegerValue(PvString(name.c_str()), out).IsOK();
    }


    bool ReadEnumFeature(PvGenParameterArray *params, const std::string &name, std::string &out) {
        if (params == nullptr) {
            return false;
        }
        PvString v;
        if (!params->GetEnumValue(PvString(name.c_str()), v).IsOK()) {
            return false;
        }
        out = ToStd(v);
        return true;
    }


    bool ReadFeatureAsString(PvGenParameterArray *params, const std::string &name, std::string &out) {
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
        out = ToStd(v);
        return true;
    }


    bool ReadFloatFeature(PvGenParameterArray *params, const std::string &name, double &out) {
        return params != nullptr && params->GetFloatValue(PvString(name.c_str()), out).IsOK();
    }


    bool ReadFloatBounds(PvGenParameterArray *params, const std::string &name, double &min_out, double &max_out) {
        if (params == nullptr) {
            return false;
        }
        auto *gf = dynamic_cast<PvGenFloat *>(params->Get(PvString(name.c_str())));
        if (gf == nullptr || !gf->IsReadable()) {
            return false;
        }
        return gf->GetMin(min_out).IsOK() && gf->GetMax(max_out).IsOK();
    }


    bool ReadEnumIntFeature(PvGenParameterArray *params, const std::string &name, int64_t &out) {
        if (params == nullptr) {
            return false;
        }
        auto *ge = dynamic_cast<PvGenEnum *>(params->Get(PvString(name.c_str())));
        return ge != nullptr && ge->IsReadable() && ge->GetValue(out).IsOK();
    }


    bool WriteEnumFeature(PvGenParameterArray *params, const std::string &name, int64_t value) {
        if (params == nullptr) {
            return false;
        }
        auto *ge = dynamic_cast<PvGenEnum *>(params->Get(PvString(name.c_str())));
        return ge != nullptr && ge->IsWritable() && ge->SetValue(value).IsOK();
    }


    bool ExecuteCommandFeature(PvGenParameterArray *params, const std::string &name) {
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
            Disconnect();
        } catch (const std::exception &e) {
            g_log.Warn("[{}] [eBUS] Disconnect during teardown failed: {}", camera_id_, e.what());
        }
    }


    void CameraController::Connect(const std::string &target, DiscoveredDevice identity) {
        identity_ = std::move(identity);
        device_ = std::make_unique<PvDeviceGEV>();
        g_log.Info("[{}] [eBUS] Connecting to {} ...", camera_id_, target);
        CHECK_PV(device_->Connect(PvString(target.c_str()), PvAccessControl), "PvDeviceGEV::Connect");

        // Direct dials skip discovery
        if (identity_.model.empty()) {
            ReadFeatureAsString(Params(), "DeviceModelName", identity_.model);
        }
        if (identity_.serial.empty()) {
            ReadFeatureAsString(Params(), "DeviceSerialNumber", identity_.serial);
        }
        if (identity_.vendor.empty()) {
            ReadFeatureAsString(Params(), "DeviceVendorName", identity_.vendor);
        }
        if (identity_.firmware.empty() && !ReadFeatureAsString(Params(), "DeviceVersion", identity_.firmware)) {
            ReadFeatureAsString(Params(), "DeviceFirmwareVersion", identity_.firmware);
        }
        if (identity_.user_name.empty()) {
            ReadFeatureAsString(Params(), "DeviceUserID", identity_.user_name);
        }

        PvResult r = device_->RegisterEventSink(this);
        if (!r.IsOK()) {
            g_log.Warn("[{}] [eBUS] RegisterEventSink failed (link-loss detection degraded): {}", camera_id_,
                       PvResultToString(r));
        }
    }


    FactoryLoadResult CameraController::LoadFactoryDefaults() {
        if (!device_) {
            throw SdkError("load_factory_defaults: device not connected");
        }
        PvGenParameterArray *p = Params();
        const auto t0 = std::chrono::steady_clock::now();

        // Manual p.165: UserSetSelector {Default, User1..3}, UserSetLoad; Default
        // is the read-only factory set (p.33), loading it restores the factory
        // defaults (p.166). IP settings live in TransportLayerControl, not in
        // user sets.
        PvGenEnum *selector = p->GetEnum(PvString("UserSetSelector"));
        PvGenCommand *load = p->GetCommand(PvString("UserSetLoad"));
        if (!ParameterUsable(selector) || !ParameterUsable(load)) {
            throw SdkError(std::string("UserSetControl not usable (UserSetSelector ") + DescribeParameter(selector) +
                           ", UserSetLoad " + DescribeParameter(load) + ")");
        }
        const auto entries = EnumEntryNames(selector);
        const auto target = gox::PickDefaultUserSetEntry(entries);
        if (!target) {
            throw SdkError("UserSetSelector has no 'Default' entry (available: " + AvailableEnumEntries(selector) + ")");
        }

        // Manual p.34: user sets load only while acquisition is stopped — a
        // session that died mid-stream leaves the camera acquiring.
        if (ExecuteCommandFeature(p, "AcquisitionStop")) {
            g_log.Trace("[{}] [eBUS] AcquisitionStop sent before the user set load", camera_id_);
        }

        CommandTimeoutGuard guard(device_->GetCommunicationParameters(), camera_id_);
        CHECK_PV(selector->SetValue(PvString(target->c_str())), "UserSetSelector");
        CHECK_PV(load->Execute(), "UserSetLoad");
        FactoryLoadResult result;
        result.selector_entry = *target;
        result.is_done_supported = WaitCommandDone(load, "UserSetLoad", camera_id_);

        // The node cache does not know the device values changed
        CHECK_PV(p->InvalidateCache(), "PvGenParameterArray::InvalidateCache");
        PvString readback;
        CHECK_PV(selector->GetValue(readback), "UserSetSelector readback");
        if (ToStd(readback) != *target) {
            throw SdkError("UserSetSelector reads back '" + ToStd(readback) + "' after the load, expected '" +
                           *target + "'");
        }

        const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        g_log.Info("[{}] [eBUS] Factory defaults loaded (UserSetSelector={}, {} ms)", camera_id_, *target, elapsed_ms);
        result.loaded = true;
        result.selector_readback = ToStd(readback);
        result.elapsed_ms = static_cast<uint64_t>(elapsed_ms);
        return result;
    }


    void CameraController::Disconnect() {
        if (!device_) {
            return;
        }
        device_->UnregisterEventSink(this);
        if (device_->IsConnected()) {
            device_->Disconnect();
        }
        device_.reset();
    }


    bool CameraController::Connected() const {
        return device_ && device_->IsConnected();
    }


    PvGenParameterArray *CameraController::Params() {
        return device_ ? device_->GetParameters() : nullptr;
    }


    uint32_t CameraController::PayloadSize() {
        if (!device_) {
            throw SdkError("payload_size: device not connected");
        }
        return device_->GetPayloadSize();
    }


    std::vector<AppliedFeature> CameraController::ApplyConfig(const CameraConfig &cfg) {
        const std::string context = "[" + camera_id_ + "] [eBUS] device";
        const std::vector<FeatureWrite> plan = BuildApplyPlan(cfg);
        std::vector<AppliedFeature> applied;
        applied.reserve(plan.size());
        for (const FeatureWrite &f: plan) {
            AppliedFeature a;
            a.name = f.name;
            a.requested = f.value;
            a.strict = f.strict;
            if (!f.required && !FeatureExists(Params(), f.name)) {
                // A bonus feature this camera does not implement: skipped, not
                // failed. Recorded so device.json shows why the metric is missing.
                a.readback = "<absent>";
                g_log.Trace("[{}] [eBUS] {} skipped (not present on this camera)", camera_id_, f.name);
            } else if (!apply_genicam_feature(Params(), f, context, f.required, &a.readback, &a.tolerance_rel)) {
                a.readback = "<failed>"; // only reachable for non-required writes
            } else {
                g_log.Trace("[{}] [eBUS] {} = {}{}", camera_id_, f.name, f.value,
                            a.readback.empty() ? "" : " (readback " + a.readback + ")");
            }
            applied.push_back(std::move(a));
        }
        g_log.Info("[{}] [eBUS] device configuration applied ({} feature writes)", camera_id_, plan.size());
        return applied;
    }


    bool CameraController::ResetTriggerCounter() {
        return ExecuteCommandFeature(Params(), "CounterReset");
    }


    DeviceReport CameraController::CollectDeviceReport(const CameraConfig &cfg, std::vector<AppliedFeature> applied,
                                                         const PtpSummary &ptp, const FactoryLoadResult &factory_load,
                                                         const RuntimeShape &runtime) {
        DeviceReport r;
        r.camera_id = camera_id_;
        r.ip = identity_.ip;
        r.connection_path = cfg.device.mac.empty() ? "direct_ip" : "mac_discovery";
        r.created_realtime_ns = common::TimeUtil::RealtimeNowNs();
        r.factory_load = factory_load;
        r.runtime = runtime;
        r.runtime.counter0_bound = Counter0Bound(applied);
        r.ptp = ptp;
        r.applied = std::move(applied);

        PvGenParameterArray *p = Params();

        // Identity is re-read in full: Connect() only fills what discovery left
        // empty, and it folds DeviceVersion and DeviceFirmwareVersion into one
        // field whose source depends on the connection path.
        for (const char *name: IdentityFeatures()) {
            std::string value;
            if (ReadFeatureAsString(p, name, value)) {
                r.identity.emplace_back(name, value);
            }
        }

        // Raw audit: the features the driver never writes. A deviation means
        // the factory baseline did not take, or the camera does not default
        // where the manual says it does - either way the dataset is not what it
        // claims and the operator needs to know. Absent/unreadable is normal
        // (mono-only and RGB-only features) and stays silent.
        // `label` is what the entry is called in device.json, `feature` the node
        // to read: they differ only for the selector-scoped reads below.
        const auto record = [&](const std::string &label, const std::string &feature, const char *expected,
                                bool reachable) {
            AuditEntry e;
            e.name = label;
            e.expected = expected;
            std::string value;
            const bool readable = reachable && ReadFeatureAsString(p, feature, value);
            e.value = readable ? value : "<absent>";
            e.outcome = ClassifyAudit(e.value, e.expected, readable);
            if (e.outcome == AuditOutcome::kDeviates) {
                g_log.Warn("[{}] [eBUS] raw audit: {} reads \"{}\", the factory value is \"{}\" - "
                           "recorded pixels may not be raw", camera_id_, label, e.value, e.expected);
            }
            r.raw_audit.push_back(std::move(e));
        };
        for (const RawAuditExpectation &x: RawAuditExpectations()) {
            record(x.name, x.name, x.expected, /*reachable=*/true);
        }

        // Selector-scoped reads. The selector is an index, not a setting, so
        // moving it is not a configuration change - but it must be put back, and
        // that is asserted: "the driver never leaves a selector moved" is what
        // makes the plain Gain/BlackLevel reads above mean AnalogAll/All.
        for (const SelectorAuditExpectation &x: SelectorAuditExpectations()) {
            int64_t entry = 0;
            const bool selected = ParseInt64(x.entry, entry) && WriteEnumFeature(p, x.selector, entry);
            record(x.label, x.feature, x.expected, selected);
        }
        for (const auto &home: SelectorHomeEntries()) {
            int64_t entry = 0;
            if (!FeatureExists(p, home.first) || !ParseInt64(home.second, entry)) {
                continue;
            }
            int64_t rb = -1;
            if (!WriteEnumFeature(p, home.first, entry) || !ReadEnumIntFeature(p, home.first, rb) ||
                rb != entry) {
                r.runtime.selector_restore_failed = true;
                g_log.Warn("[{}] [eBUS] {} could not be restored to entry {} after the raw audit; "
                           "selector-scoped values in device.json may name the wrong channel", camera_id_,
                           home.first, home.second);
            }
        }

        for (const char *name: TransportFeatures()) {
            std::string value;
            if (ReadFeatureAsString(p, name, value)) {
                r.transport.emplace_back(name, value);
            }
        }

        double lo = 0, hi = 0;
        if (ReadFloatBounds(p, "AcquisitionFrameRate", lo, hi)) {
            r.frame_rate_min_hz = lo;
            r.frame_rate_max_hz = hi;
        }
        double exposure = 0;
        if (ReadFloatFeature(p, "ExposureTime", exposure)) {
            r.exposure_time_us = exposure;
        }
        if (ReadFloatBounds(p, "ExposureTime", lo, hi)) {
            r.exposure_time_min_us = lo;
            r.exposure_time_max_us = hi;
        }
        std::string black_level;
        if (ReadFeatureAsString(p, "BlackLevel", black_level)) {
            r.black_level_register = black_level;
        }
        // The A/D depth the driver decided on from the pixel format; the
        // register's own text is in the audit block.
        if (cfg.acquisition.pixel_format) {
            const int bits = PixelFormatBitDepth(*cfg.acquisition.pixel_format);
            if (bits != 0) {
                r.sensor_digitization_bits = bits;
            }
        }
        return r;
    }


    TelemetrySample CameraController::SampleTelemetry(bool want_counter) {
        TelemetrySample s;
        s.hrt = common::TimeUtil::RealtimeNowNs();
        PvGenParameterArray *p = Params();
        if (p == nullptr) {
            return s;
        }
        if (!telemetry_probed_) {
            telemetry_probed_ = true;
            have_temperature_ = FeatureExists(p, "DeviceTemperature") &&
                                FeatureExists(p, "DeviceTemperatureSelector");
            have_counter_ = FeatureExists(p, "CounterValue");
            have_pause_counter_ = FeatureExists(p, "aPAUSEMACCtrlFramesReceived");
        }

        if (have_temperature_) {
            // DeviceTemperatureSelector {0: Mainboard, 1: Sensor, 2: FPGA}
            // (manual p.125). The selector only chooses which sensor
            // DeviceTemperature reports; it changes nothing about the capture.
            struct Probe {
                int64_t entry;
                std::optional<double> *out;
            };
            const Probe probes[] = {
                {1, &s.temp_sensor},
                {0, &s.temp_mainboard},
                {2, &s.temp_fpga},
            };
            int64_t previous_entry = 0;
            const bool restore = ReadEnumIntFeature(p, "DeviceTemperatureSelector", previous_entry);
            for (const Probe &probe: probes) {
                double value = 0;
                if (WriteEnumFeature(p, "DeviceTemperatureSelector", probe.entry) &&
                    ReadFloatFeature(p, "DeviceTemperature", value)) {
                    *probe.out = value;
                } else if (!temp_warned_) {
                    temp_warned_ = true;
                    g_log.Warn("[{}] [eBUS] device temperature is not readable through "
                               "DeviceTemperatureSelector; telemetry records null for it", camera_id_);
                }
            }
            if (restore) {
                WriteEnumFeature(p, "DeviceTemperatureSelector", previous_entry); // never leave a selector moved
            }
        }

        if (want_counter && have_counter_) {
            int64_t value = 0;
            if (ReadIntFeature(p, "CounterValue", value)) {
                s.trig = value;
            }
            // CounterStatus 4 = CounterOverflow (manual p.160): the counter is
            // 32-bit, so a long external-trigger session can wrap it.
            std::string status;
            if (ReadFeatureAsString(p, "CounterStatus", status)) {
                s.trig_overflow = common::StringUtil::EqualsCi(common::StringUtil::Trim(status), "CounterOverflow");
            }
        }

        if (have_pause_counter_) {
            int64_t value = 0;
            if (ReadIntFeature(p, "aPAUSEMACCtrlFramesReceived", value)) {
                s.pause_rx = value;
            }
        }
        return s;
    }


    void CameraController::StreamEnable() {
        if (!device_) {
            throw SdkError("stream_enable: device not connected");
        }
        CHECK_PV(device_->StreamEnable(), "PvDevice::StreamEnable");
    }

    void CameraController::StreamDisable(bool ignore_errors) {
        if (!device_) {
            return;
        }
        PvResult r = device_->StreamDisable();
        if (!r.IsOK()) {
            if (!ignore_errors && !LinkLost()) {
                throw SdkError("PvDevice::StreamDisable", r);
            }
            g_log.Trace("[{}] [eBUS] StreamDisable ignored failure: {}", camera_id_, PvResultToString(r));
        }
    }


    void CameraController::AcquisitionStart() {
        if (!device_) {
            throw SdkError("acquisition_start: device not connected");
        }
        auto *cmd = dynamic_cast<PvGenCommand *>(Params()->Get(PvString("AcquisitionStart")));
        if (cmd == nullptr) {
            throw SdkError("AcquisitionStart command not found on device");
        }
        CHECK_PV(cmd->Execute(), "AcquisitionStart");
        g_log.Info("[{}] [eBUS] AcquisitionStart executed", camera_id_);
    }


    void CameraController::AcquisitionStop(bool ignore_errors) {
        if (!device_ || !device_->IsConnected()) {
            return;
        }
        auto *cmd = dynamic_cast<PvGenCommand *>(Params()->Get(PvString("AcquisitionStop")));
        if (cmd == nullptr) {
            if (ignore_errors) {
                return;
            }
            throw SdkError("AcquisitionStop command not found on device");
        }
        PvResult r = cmd->Execute();
        if (!r.IsOK()) {
            if (!ignore_errors && !LinkLost()) {
                throw SdkError("AcquisitionStop", r);
            }
            g_log.Debug("[{}] [eBUS] AcquisitionStop ignored failure: {}", camera_id_, PvResultToString(r));
        } else {
            g_log.Info("[{}] [eBUS] AcquisitionStop executed", camera_id_);
        }
    }


    void CameraController::OnLinkDisconnected(PvDevice *) {
        link_lost_.store(true, std::memory_order_relaxed);
        g_log.Error(
            "[{}] [eBUS] device link lost; stopping to preserve captured data (automatic reconnect is not implemented in v1)",
            camera_id_);
        if (stop_ != nullptr) {
            stop_->RequestStop(StopReason::kError);
        }
    }
} // namespace gox::ebus
