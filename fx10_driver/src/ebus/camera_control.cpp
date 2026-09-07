#include "ebus/camera_control.h"
#include "ebus/stream_receiver.h"

#include <PvDevice.h>
#include <PvGenBoolean.h>
#include <PvGenCommand.h>
#include <PvGenEnum.h>
#include <PvGenInteger.h>
#include <PvGenParameterArray.h>
#include <PvGenParameter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string_view>
#include <thread>

#include "logger.h"
#include "ebus/sdk_error.h" // common/ (amiga_ebus)


namespace fx10 {
    namespace {
        common::DriverLog g_log{"FX10"};

        // UserSetLoad is a flash read: raise the GVCP answer timeout like the
        // SDK's own eBUS Player does (UserSetsManager.cpp). Completion/readiness
        // is checked by refreshed parameter reads, not a command-register poll.
        constexpr std::int64_t kUserSetAnswerTimeoutMs = 10000;
        constexpr int kControlReadyTimeoutMs = 15000;
        constexpr int kControlReadyPollMs = 200;


        // PvString -> std::string via the shared NULL-safe helper (common/).
        std::string pv(const PvString &s) { return common::Ebus::ToStd(s); }


        [[noreturn]] void failNode(const std::string &node, const std::string &what) {
            throw ControlError("[eBUS] GenICam node '" + node + "': " + what);
        }


        template<typename T>
        T *NodeAs(PvGenParameterArray *array, const std::string &node) {
            PvGenParameter *parameter = array->Get(PvString(node.c_str()));
            if (parameter == nullptr) {
                failNode(node, "not found in the node map");
            }
            T *typed = dynamic_cast<T *>(parameter);
            if (typed == nullptr) {
                failNode(node, "has a different GenICam type than requested");
            }
            if (!parameter->IsAvailable()) {
                failNode(node, "not available in the current camera state");
            }
            return typed;
        }


        void Check(const PvResult &result, const std::string &node, const char *op) {
            if (!result.IsOK()) {
                failNode(node, std::string(op) + " failed: " + pv(result.GetCodeString()));
            }
        }


        const char *TypeName(PvGenType type) {
            switch (type) {
                case PvGenTypeInteger: return "int";
                case PvGenTypeEnum: return "enum";
                case PvGenTypeBoolean: return "bool";
                case PvGenTypeString: return "string";
                case PvGenTypeCommand: return "command";
                case PvGenTypeFloat: return "float";
                default: return "other";
            }
        }
    } // namespace


    CameraControl::CameraControl(PvDevice &device) : device_(device) {
    }


    // --- direct typed access -----------------------------------------------------
    std::int64_t CameraControl::GetInt(const std::string &node) const {
        std::int64_t value = 0;
        Check(NodeAs<PvGenInteger>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return value;
    }


    double CameraControl::GetFloat(const std::string &node) const {
        double value = 0.0;
        Check(NodeAs<PvGenFloat>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return value;
    }


    bool CameraControl::GetBool(const std::string &node) const {
        bool value = false;
        Check(NodeAs<PvGenBoolean>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return value;
    }


    std::string CameraControl::GetEnum(const std::string &node) const {
        PvString value;
        Check(NodeAs<PvGenEnum>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return pv(value);
    }


    std::string CameraControl::GetString(const std::string &node) const {
        PvString value;
        Check(NodeAs<PvGenString>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return pv(value);
    }


    std::int64_t CameraControl::SetInt(const std::string &node, std::int64_t value) const {
        Check(NodeAs<PvGenInteger>(device_.GetParameters(), node)->SetValue(value), node, "write");
        const std::int64_t actual = GetInt(node);
        if (actual != value) {
            g_log.Warn("[eBUS] {} = {} requested, camera clamped to {}", node, value, actual);
        }
        g_log.Trace("[eBUS] {} has been set to {}", node, actual);
        return actual;
    }


    double CameraControl::SetFloat(const std::string &node, double value) {
        Check(NodeAs<PvGenFloat>(device_.GetParameters(), node)->SetValue(value), node, "write");
        const double actual = GetFloat(node);
        const double tolerance = std::max(std::fabs(value) * 1e-3, 1e-9);
        if (std::fabs(actual - value) > tolerance) {
            g_log.Warn("[eBUS] {} = {} requested, camera clamped to {}", node, value, actual);
        }
        g_log.Trace("[eBUS] {} has been set to {}", node, actual);
        return actual;
    }


    void CameraControl::SetBool(const std::string &node, bool value) {
        Check(NodeAs<PvGenBoolean>(device_.GetParameters(), node)->SetValue(value), node, "write");
        bool actual = GetBool(node);
        if (actual != value) {
            failNode(node, "read-back mismatch after boolean write");
        }
        g_log.Trace("[eBUS] {} has been set to {}", node, actual);
    }


    void CameraControl::SetEnum(const std::string &node, const std::string &entry) {
        Check(NodeAs<PvGenEnum>(device_.GetParameters(), node)->SetValue(PvString(entry.c_str())),
              node, ("write '" + entry + "'").c_str());
        const std::string actual = GetEnum(node);
        if (actual != entry) {
            failNode(node, "read-back mismatch: wrote '" + entry + "', camera reports '" + actual + "'");
        }
        g_log.Trace("[eBUS] {} has been set to {}", node, actual);
    }


    void CameraControl::SetString(const std::string &node, const std::string &value) {
        Check(NodeAs<PvGenString>(device_.GetParameters(), node)->SetValue(PvString(value.c_str())),
              node, "write");
        const std::string actual = GetString(node);
        if (actual != value) {
            failNode(node, "read-back mismatch: wrote '" + value + "', camera reports '" + actual + "'");
        }
        g_log.Trace("[eBUS] {} has been set to {}", node, actual);
    }


    void CameraControl::Execute(const std::string &node) {
        Check(NodeAs<PvGenCommand>(device_.GetParameters(), node)->Execute(), node, "execute");
    }


    // --- factory defaults ----------------------------------------------------------
    namespace {
        // Raises AnswerTimeout and suppresses disconnect-on-timeout for the
        // duration of a slow command; restores both on every exit path.
        class CommandTimeoutGuard {
        public:
            explicit CommandTimeoutGuard(PvGenParameterArray *comm) {
                if (comm == nullptr) {
                    throw ControlError("[eBUS] communication parameters unavailable");
                }
                answer_ = comm->GetInteger(PvString("AnswerTimeout"));
                if (answer_ == nullptr || !answer_->GetValue(old_answer_).IsOK()) {
                    throw ControlError("[eBUS] AnswerTimeout communication parameter unavailable");
                }
                Check(answer_->SetValue(std::max(old_answer_, kUserSetAnswerTimeoutMs)), "AnswerTimeout", "write");
                disconnect_ = comm->GetBoolean(PvString("DisconnectOnAnyTimeout"));
                if (disconnect_ != nullptr && disconnect_->GetValue(old_disconnect_).IsOK()) {
                    have_disconnect_ = disconnect_->SetValue(false).IsOK();
                }
            }

            ~CommandTimeoutGuard() {
                if (!answer_->SetValue(old_answer_).IsOK()) {
                    g_log.Warn("[eBUS] AnswerTimeout could not be restored to {} ms", old_answer_);
                }
                if (have_disconnect_ && !disconnect_->SetValue(old_disconnect_).IsOK()) {
                    g_log.Warn("[eBUS] DisconnectOnAnyTimeout could not be restored");
                }
            }

            CommandTimeoutGuard(const CommandTimeoutGuard &) = delete;
            CommandTimeoutGuard &operator=(const CommandTimeoutGuard &) = delete;

        private:
            PvGenInteger *answer_ = nullptr;
            PvGenBoolean *disconnect_ = nullptr;
            std::int64_t old_answer_ = 0;
            bool old_disconnect_ = false;
            bool have_disconnect_ = false;
        };


    } // namespace


    void CameraControl::ExecuteFactoryCommand(const char *name, const std::function<bool()> &stop) const {
        PvGenParameterArray *p = device_.GetParameters();

        // Missing reset nodes are fatal. An acknowledged command and a healthy
        // control link still do not prove every undocumented factory value.
        PvGenCommand *command = p->GetCommand(PvString(name));
        if (command == nullptr) {
            failNode(name, "not found in fresh node map");
        }

        // User sets load only while acquisition is stopped
        if (PvGenCommand *acquisition_stop = p->GetCommand(PvString("AcquisitionStop")); acquisition_stop != nullptr) {
            const auto stop_start = std::chrono::steady_clock::now();
            const auto result = acquisition_stop->Execute();
            g_log.Trace("[eBUS] AcquisitionStop before {}: {} ({} ms)", name, pv(result.GetCodeString()),
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - stop_start).count());
        }

        if (stop && stop()) {
            throw ControlError("[eBUS] factory preparation interrupted");
        }
        CommandTimeoutGuard guard(device_.GetCommunicationParameters());
        if (std::string_view(name) == "UserSetLoad") {
            if (auto *selector = p->GetEnum(PvString("UserSetSelector")); selector != nullptr) {
                PvString selected;
                const auto selected_result = selector->GetValue(selected);
                g_log.Info("[eBUS] UserSetLoad selector={} readback={} detail={}",
                           selected_result.IsOK() ? pv(selected) : "<unreadable>",
                           pv(selected_result.GetCodeString()), pv(selected_result.GetDescription()));
            } else {
                g_log.Warn("[eBUS] UserSetSelector absent; loaded set cannot be identified by this node");
            }
        }
        const auto execute_start = std::chrono::steady_clock::now();
        const PvResult result = command->Execute();
        g_log.Trace("[eBUS] {} Execute result={} detail={} elapsed={} ms", name, pv(result.GetCodeString()),
                   pv(result.GetDescription()),
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - execute_start).count());
        // An unacknowledged command is ambiguous: never automatically replay it.
        Check(result, name, "execute (not retried)");
        // eBUS 6.5.1 UserSetsManager::Load also avoids IsDone. On this FX10
        // IsDone consumed ~10 s immediately before a link-loss notification.
        // Execute success is not a readiness guarantee: the caller polls fresh
        // geometry before proceeding, without reconnecting or replaying commands.
    }


    void CameraControl::VerifyControlReady() const {
        if (!device_.IsConnected()) {
            throw ControlError("[eBUS] control connection is not established");
        }
        Check(device_.GetParameters()->InvalidateCache(), "node cache", "invalidate");
        const auto width = GetInt("Width");
        const auto height = GetInt("Height");
        const auto payload = GetInt("PayloadSize");
        const auto format = GetEnum("PixelFormat");
        if (width <= 0 || height <= 0 || payload <= 0 || format.empty()) {
            throw ControlError("[eBUS] fresh geometry readback is not usable");
        }
        g_log.Info("[eBUS] Control ready: fresh readback {}x{}, {} bytes, {}", width, height, payload, format);
    }


    bool PrepareFactoryDefaults(StreamReceiver &receiver, const std::function<bool()> &stop) {
        const auto cancelled = [&] { return stop && stop(); };
        for (const char *name: {"CameraHeadFactoryReset", "UserSetLoad"}) {
            if (cancelled()) {
                return false;
            }
            if (receiver.Device() == nullptr || !receiver.Device()->IsConnected() || receiver.Failed()) {
                throw ControlError(std::string("[eBUS] ") + name + ": no healthy control connection");
            }
            {
                CameraControl control(*receiver.Device());
                try {
                    control.ExecuteFactoryCommand(name, stop);
                } catch (...) {
                    if (cancelled()) {
                        return false;
                    }
                    throw;
                }
            }
            if (cancelled()) {
                return false;
            }
            CameraControl control(*receiver.Device());
            const auto started = std::chrono::steady_clock::now();
            const auto deadline = started + std::chrono::milliseconds(kControlReadyTimeoutMs);
            auto next_log = started;
            std::string last_error = "no usable readback";
            for (;;) {
                if (cancelled()) {
                    return false;
                }
                if (!receiver.Device()->IsConnected() || receiver.Failed()) {
                    throw ControlError(std::string("[eBUS] ") + name + ": control link lost while waiting for readiness");
                }
                bool ready = false;
                try {
                    control.VerifyControlReady();
                    ready = true;
                } catch (const ControlError &e) {
                    last_error = e.what();
                }
                const auto now = std::chrono::steady_clock::now();
                if (cancelled()) {
                    return false;
                }
                if (!receiver.Device()->IsConnected() || receiver.Failed()) {
                    throw ControlError(std::string("[eBUS] ") + name + ": control link lost during readback");
                }
                if (now >= deadline) {
                    throw ControlError(std::string("[eBUS] ") + name + " control readiness exceeded " +
                                       std::to_string(kControlReadyTimeoutMs) + " ms: " + last_error);
                }
                if (ready) {
                    g_log.Info("[eBUS] {} control readiness verified after {} ms", name,
                               std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
                    break;
                }
                if (now >= next_log) {
                    g_log.Trace("[eBUS] {} waiting for control readiness: {}", name, last_error);
                    next_log = now + std::chrono::seconds(2);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kControlReadyPollMs));
            }
        }
        g_log.Info("[eBUS] Factory preparation finished; control readback verified; applying acquisition configuration next");
        return true;
    }


    // --- best-effort telemetry reads ---------------------------------------------------
    std::optional<double> CameraControl::TryReadTemperature(const std::string &selector_entry) {
        try {
            SetEnum(node::kDeviceTemperatureSelector, selector_entry);
            return GetFloat(node::kDeviceTemperature);
        } catch (const ControlError &e) {
            g_log.Debug("[eBUS] tryReadTemperature({}): {}", selector_entry, e.what());
            return std::nullopt;
        }
    }


    std::optional<std::int64_t> CameraControl::TryGetInt(const std::string &node) const {
        try {
            return GetInt(node);
        } catch (const ControlError &e) {
            g_log.Debug("[eBUS] tryGetInt({}): {}", node, e.what());
            return std::nullopt;
        }
    }


    std::optional<double> CameraControl::TryGetFloat(const std::string &node) const {
        try {
            return GetFloat(node);
        } catch (const ControlError &e) {
            g_log.Debug("[eBUS] tryGetFloat({}): {}", node, e.what());
            return std::nullopt;
        }
    }


    // --- configuration application -------------------------------------------------
    // The achievable frame rate is 1/(exposure + readout) and the node's live
    // [min, max] already reflects the exposure written earlier in the plan. The
    // FX10e REJECTS an out-of-range write with GENERIC_ERROR instead of
    // clamping, so clamp before writing.
    double CameraControl::ClampToNodeRange(const std::string &node, double value) {
        auto *typed = dynamic_cast<PvGenFloat *>(device_.GetParameters()->Get(PvString(node.c_str())));
        double lo = 0.0, hi = 0.0;
        if (typed == nullptr || !typed->GetMin(lo).IsOK() || !typed->GetMax(hi).IsOK() || lo > hi) {
            return value;
        }
        const double clamped = std::min(std::max(value, lo), hi);
        if (clamped != value) {
            g_log.Warn("[eBUS] {} {:.3f} is outside the camera's current range [{:.3f}, {:.3f}] "
                       "(exposure-limited); using {:.3f}", node, value, lo, hi, clamped);
        }
        return clamped;
    }


    bool CameraControl::ApplyWrite(const FeatureWrite &write) {
        const std::string &node = write.node;
        // An optional write whose node this firmware does not implement is skipped
        if (!write.required && device_.GetParameters()->Get(PvString(node.c_str())) == nullptr) {
            g_log.Warn("[eBUS] '{}' is not present on this camera - skipped", node);
            return false;
        }

        try {
            switch (write.kind) {
                case WriteKind::kInt: {
                    const std::int64_t actual = SetInt(node, write.int_value);
                    if (actual != write.int_value && write.strict) {
                        failNode(node, "camera clamped " + std::to_string(write.int_value) + " to " +
                                       std::to_string(actual) +
                                       "; the requested value is not achievable with the current settings");
                    }
                    break;
                }
                case WriteKind::kFloat: {
                    double value = write.float_value;
                    if (write.clamp_to_node_range) {
                        value = ClampToNodeRange(node, value);
                    }
                    const double actual = SetFloat(node, value);
                    // 0.1 % covers the node's own step quantisation.
                    if (write.strict && std::fabs(actual - value) > std::max(std::fabs(value) * 1e-3, 1e-9)) {
                        failNode(node, "camera clamped " + std::to_string(value) + " to " +
                                       std::to_string(actual) +
                                       "; the requested value is not achievable with the current settings");
                    }
                    break;
                }
                case WriteKind::kBool:
                    SetBool(node, write.bool_value);
                    break;
                case WriteKind::kEnum:
                    SetEnum(node, write.text);
                    break;
                case WriteKind::kString:
                    SetString(node, write.text);
                    break;
                case WriteKind::kCommand:
                    Execute(node);
                    break;
            }
        } catch (const ControlError &e) {
            if (write.required) {
                throw;
            }
            g_log.Warn("[eBUS] optional write '{}' failed ({}) - skipped", node, e.what());
            return false;
        }
        return true;
    }


    void CameraControl::ApplyAcquisitionConfig(const AcquisitionConfig &acquisition,
                                               const std::vector<RawFeature> &raw) {
        const std::vector<FeatureWrite> plan = BuildApplyPlan(acquisition, raw);
        bool line_selector_written = false;
        for (const FeatureWrite &write: plan) {
            // LineSelector/Line1 is not confirmed by the FX10 reference manual.
            // A device without that node can expose LineSource directly. If
            // the node exists, never rely on an inherited selector value.
            if (write.node == "LineSource" && !line_selector_written &&
                device_.GetParameters()->Get(PvString("LineSelector")) != nullptr) {
                failNode("LineSource", "this camera exposes LineSelector; precede LineSource in features.raw "
                         "with the verified strobe LineSelector entry from this camera's node map");
            }
            const bool applied = ApplyWrite(write);
            if (write.node == "LineSelector" && applied) {
                line_selector_written = true;
            }
        }
        // Raw overrides run last. Recheck values the receiver/calibration logic
        // relies on so an override cannot silently invalidate that contract.
        Check(device_.GetParameters()->InvalidateCache(), "node cache", "invalidate after configuration");
        if (GetEnum(node::kPixelFormat) != acquisition.pixel_format ||
            GetInt(node::kSpatialBinning) != acquisition.spatial_binning ||
            GetInt(node::kSpectralBinning) != acquisition.spectral_binning ||
            GetBool(node::kStatusLine) != acquisition.status_line ||
            GetBool(node::kMroiEnable) != acquisition.mroi.enabled) {
            throw ControlError("[eBUS] final image configuration disagrees with acquisition settings; "
                               "remove conflicting features.raw overrides");
        }
        g_log.Info("[eBUS] Acquisition configuration applied ({} feature writes)", plan.size());
    }


    CameraControl::Geometry CameraControl::ReadGeometry() {
        Geometry geometry;
        geometry.width = GetInt("Width");
        geometry.height = GetInt("Height");
        geometry.offset_x = TryGetInt("OffsetX").value_or(-1);
        geometry.offset_y = TryGetInt("OffsetY").value_or(-1);
        geometry.payload_size = device_.GetPayloadSize();
        geometry.pixel_format = GetEnum("PixelFormat");
        g_log.Info("[eBUS] Camera geometry: {} samples x {} bands, {} (payload {} B/line)", geometry.width,
                   geometry.height,
                   geometry.pixel_format, geometry.payload_size);
        return geometry;
    }
} // namespace fx10
