#include "ebus/camera_control.hpp"

#include <PvDevice.h>
#include <PvGenParameterArray.h>
#include <PvGenParameter.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include "logger.h"


namespace fx10 {
    namespace {
        Common::DriverLog g_log{"FX10"};


        std::string pv(const PvString &s) { return std::string(s.GetAscii()); }


        [[noreturn]] void failNode(const std::string &node, const std::string &what) {
            throw ControlError("[eBUS] GenICam node '" + node + "': " + what);
        }


        template<typename T>
        T *nodeAs(PvGenParameterArray *array, const std::string &node) {
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


        void check(const PvResult &result, const std::string &node, const char *op) {
            if (!result.IsOK()) {
                failNode(node, std::string(op) + " failed: " + pv(result.GetCodeString()));
            }
        }


        const char *typeName(PvGenType type) {
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


    CameraControl::CameraControl(PvDevice &device, const FeatureConfig &features)
        : device_(device), features_(features) {
    }


    // --- direct typed access -----------------------------------------------------
    std::int64_t CameraControl::getInt(const std::string &node) {
        std::int64_t value = 0;
        check(nodeAs<PvGenInteger>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return value;
    }


    double CameraControl::getFloat(const std::string &node) {
        double value = 0.0;
        check(nodeAs<PvGenFloat>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return value;
    }


    bool CameraControl::getBool(const std::string &node) {
        bool value = false;
        check(nodeAs<PvGenBoolean>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return value;
    }


    std::string CameraControl::getEnum(const std::string &node) {
        PvString value;
        check(nodeAs<PvGenEnum>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return pv(value);
    }


    std::string CameraControl::getString(const std::string &node) {
        PvString value;
        check(nodeAs<PvGenString>(device_.GetParameters(), node)->GetValue(value), node, "read");
        return pv(value);
    }


    std::int64_t CameraControl::setInt(const std::string &node, std::int64_t value) {
        check(nodeAs<PvGenInteger>(device_.GetParameters(), node)->SetValue(value), node, "write");
        const std::int64_t actual = getInt(node);
        if (actual != value) {
            g_log.warn("[eBUS] {} = {} requested, camera clamped to {}", node, value, actual);
        }
        g_log.trace("[eBUS] {} has been set to {}", node, actual);
        return actual;
    }


    double CameraControl::setFloat(const std::string &node, double value) {
        check(nodeAs<PvGenFloat>(device_.GetParameters(), node)->SetValue(value), node, "write");
        const double actual = getFloat(node);
        const double tolerance = std::max(std::fabs(value) * 1e-3, 1e-9);
        if (std::fabs(actual - value) > tolerance) {
            g_log.warn("[eBUS] {} = {} requested, camera clamped to {}", node, value, actual);
        }
        g_log.trace("[eBUS] {} has been set to {}", node, actual);
        return actual;
    }


    void CameraControl::setBool(const std::string &node, bool value) {
        check(nodeAs<PvGenBoolean>(device_.GetParameters(), node)->SetValue(value), node, "write");
        bool actual = getBool(node);
        if (actual != value) {
            failNode(node, "read-back mismatch after boolean write");
        }
        g_log.trace("[eBUS] {} has been set to {}", node, actual);
    }


    void CameraControl::setEnum(const std::string &node, const std::string &entry) {
        check(nodeAs<PvGenEnum>(device_.GetParameters(), node)->SetValue(PvString(entry.c_str())),
              node, ("write '" + entry + "'").c_str());
        const std::string actual = getEnum(node);
        if (actual != entry) {
            failNode(node, "read-back mismatch: wrote '" + entry + "', camera reports '" + actual + "'");
        }
        g_log.trace("[eBUS] {} has been set to {}", node, actual);
    }


    void CameraControl::setString(const std::string &node, const std::string &value) {
        check(nodeAs<PvGenString>(device_.GetParameters(), node)->SetValue(PvString(value.c_str())),
              node, "write");
        const std::string actual = getString(node);
        if (actual != value) {
            failNode(node, "read-back mismatch: wrote '" + value + "', camera reports '" + actual + "'");
        }
        g_log.trace("[eBUS] {} has been set to {}", node, actual);
    }


    void CameraControl::execute(const std::string &node) {
        check(nodeAs<PvGenCommand>(device_.GetParameters(), node)->Execute(), node, "execute");
    }


    // --- role-based access ---------------------------------------------------------
    namespace {
        bool roleUnmapped(const FeatureConfig &features, const std::string &role, std::string &node) {
            node = features.node(role); // throws on unknown role (programming error)
            if (node.empty()) {
                g_log.warn(
                    "[eBUS] Feature role '{}' has no GenICam node mapped yet - skipped (fill features.map from the camera feature dump)",
                    role);
                return true;
            }
            return false;
        }
    } // namespace


    bool CameraControl::setIntByRole(const std::string &role, std::int64_t value) {
        std::string node;
        if (roleUnmapped(features_, role, node)) {
            return false;
        }
        setInt(node, value);
        return true;
    }


    bool CameraControl::setFloatByRole(const std::string &role, double value) {
        std::string node;
        if (roleUnmapped(features_, role, node)) {
            return false;
        }
        setFloat(node, value);
        return true;
    }


    bool CameraControl::setBoolByRole(const std::string &role, bool value) {
        std::string node;
        if (roleUnmapped(features_, role, node)) {
            return false;
        }
        setBool(node, value);
        return true;
    }


    bool CameraControl::setEnumByRole(const std::string &role, const std::string &entry) {
        std::string node;
        if (roleUnmapped(features_, role, node)) {
            return false;
        }
        setEnum(node, entry);
        return true;
    }


    bool CameraControl::setStringByRole(const std::string &role, const std::string &value) {
        std::string node;
        if (roleUnmapped(features_, role, node)) {
            return false;
        }
        setString(node, value);
        return true;
    }


    std::optional<std::int64_t> CameraControl::tryGetIntByRole(const std::string &role) {
        const std::string &node = features_.node(role);
        if (node.empty()) {
            return std::nullopt;
        }
        try {
            return getInt(node);
        } catch (const ControlError &e) {
            g_log.debug("[eBUS] tryGetIntByRole({}): {}", role, e.what());
            return std::nullopt;
        }
    }


    std::optional<double> CameraControl::tryGetFloatByRole(const std::string &role) {
        const std::string &node = features_.node(role);
        if (node.empty()) {
            return std::nullopt;
        }
        try {
            return getFloat(node);
        } catch (const ControlError &e) {
            g_log.debug("[eBUS] tryGetFloatByRole({}): {}", role, e.what());
            return std::nullopt;
        }
    }


    // --- configuration application -------------------------------------------------
    void CameraControl::applyAcquisitionConfig(const AcquisitionConfig &acquisition) {
        // Order matters: geometry-affecting features first, then exposure/rate/trigger
        setIntByRole("spatial_binning", acquisition.spatial_binning);
        setIntByRole("spectral_binning", acquisition.spectral_binning);
        setEnumByRole("pixel_format", acquisition.pixel_format);

        if (acquisition.mroi.enabled) {
            // FX10e MROI (Multiple Regions of Interest) is index-register based: select MROI_Index, then write Y/H
            if (features_.node("mroi_enable").empty() || features_.node("mroi_index").empty() ||
                features_.node("mroi_y").empty() || features_.node("mroi_h").empty()) {
                g_log.warn("[eBUS] MROI requested but mroi_* roles are not fully mapped - skipped");
            } else {
                const auto regions = parseMroiRegions(acquisition.mroi.multiband_string);
                for (std::size_t i = 0; i < regions.size(); ++i) {
                    setIntByRole("mroi_index", static_cast<std::int64_t>(i));
                    setIntByRole("mroi_y", regions[i].first);
                    setIntByRole("mroi_h", regions[i].second);
                }
                setBoolByRole("mroi_enable", true);
                g_log.trace("[eBUS] MROI enabled with {} region(s)", regions.size());
            }
        } else {
            setBoolByRole("mroi_enable", false);
        }

        setBoolByRole("status_line", acquisition.status_line);

        // Exposure MODE first
        // A sticky TriggerControlled left over from a previous run can make ExposureTime unavailable until the mode is reset
        // FX10e entries are {Timed, TriggerControlled}
        const bool pulse_width = acquisition.trigger.mode == TriggerMode::kExternal &&
                                 acquisition.trigger.exposure_control == ExposureControl::kPulseWidth;
        setEnumByRole("exposure_mode", pulse_width ? "TriggerControlled" : "Timed");
        if (!pulse_width) {
            // ExposureTime is in microseconds on the FX10e ([10..419000])
            setFloatByRole("exposure_time", acquisition.exposure_ms * 1000.0);
        }

        setEnumByRole("acquisition_mode", "Continuous");

        if (acquisition.trigger.mode == TriggerMode::kFreerun) {
            // Selector first so TriggerMode=Off targets the frame-start trigger
            setEnumByRole("trigger_selector", acquisition.trigger.selector_entry);
            setEnumByRole("trigger_mode", "Off");
            setBoolByRole("frame_rate_enable", true); // FX10e: EnAcquisitionFrameRate
            // The achievable rate is 1/(exposure + readout) — strictly below
            // 1000/exposure_ms — and the node's [min, max] already reflects the
            // exposure written above. The FX10e REJECTS an out-of-range write
            // with GENERIC_ERROR instead of clamping, so clamp here first.
            double rate = acquisition.frame_rate_hz;
            const std::string &rate_node = features_.node("frame_rate");
            if (!rate_node.empty()) {
                auto *node = dynamic_cast<PvGenFloat *>(device_.GetParameters()->Get(PvString(rate_node.c_str())));
                double lo = 0.0, hi = 0.0;
                if (node != nullptr && node->GetMin(lo).IsOK() && node->GetMax(hi).IsOK() && lo <= hi) {
                    const double clamped = std::min(std::max(rate, lo), hi);
                    if (clamped != rate) {
                        g_log.warn("[eBUS] AcquisitionFrameRate {:.3f} is outside the camera's current range "
                                   "[{:.3f}, {:.3f}] (exposure-limited); using {:.3f}",
                                   rate, lo, hi, clamped);
                        rate = clamped;
                    }
                }
            }
            setFloatByRole("frame_rate", rate);
        } else {
            // External PPS-per-line trigger
            if (features_.node("trigger_mode").empty() || features_.node("trigger_source").empty()) {
                throw ControlError(
                    "external trigger requested but the trigger_mode/trigger_source roles have no "
                    "GenICam node mapped; dump the camera features and fill features.map");
            }
            setEnumByRole("trigger_selector", acquisition.trigger.selector_entry);
            setEnumByRole("trigger_source", acquisition.trigger.source_entry);
            setEnumByRole("trigger_activation",
                          acquisition.trigger.activation == TriggerActivation::kRising ? "RisingEdge" : "FallingEdge");
            // TriggerDelay is in microseconds on the FX10e ([0..419000])
            // the config value is ms per the manual's convention
            setFloatByRole("trigger_delay", acquisition.trigger.delay_ms * 1000.0);
            setEnumByRole("trigger_mode", "On"); // exposure_mode was set above
        }

        // Raw passthrough LAST so explicit user settings can override anything above
        applyRawFeatures(features_.raw);
    }


    void CameraControl::applyRawFeatures(const std::vector<RawFeature> &raw) {
        for (const RawFeature &feature: raw) {
            if (feature.type == "int") {
                setInt(feature.name, std::stoll(feature.value));
            } else if (feature.type == "float") {
                setFloat(feature.name, std::stod(feature.value));
            } else if (feature.type == "bool") {
                setBool(feature.name, feature.value == "true" || feature.value == "1");
            } else if (feature.type == "enum") {
                setEnum(feature.name, feature.value);
            } else if (feature.type == "string") {
                setString(feature.name, feature.value);
            } else if (feature.type == "command") {
                execute(feature.name);
            }
        }
    }


    CameraControl::Geometry CameraControl::readGeometry() {
        Geometry geometry;
        geometry.width = getInt("Width");
        geometry.height = getInt("Height");
        geometry.payload_size = device_.GetPayloadSize();
        geometry.pixel_format = getEnum("PixelFormat");
        g_log.info("Camera geometry: {} samples x {} bands, {} (payload {} B/line)", geometry.width, geometry.height,
                   geometry.pixel_format, geometry.payload_size);
        return geometry;
    }


    // Help function
    // --- feature dump ----------------------------------------------------------------
    std::size_t CameraControl::dumpAllFeatures(std::ostream &os, PvGenParameterArray *array) {
        if (array == nullptr) {
            array = device_.GetParameters();
        }
        std::size_t dumped = 0;

        const std::uint32_t count = array->GetCount();
        for (std::uint32_t i = 0; i < count; ++i) {
            PvGenParameter *parameter = array->Get(i);
            if (parameter == nullptr) {
                continue;
            }

            PvString name_pv;
            parameter->GetName(name_pv);
            PvString category_pv;
            parameter->GetCategory(category_pv);
            PvGenType type = PvGenTypeUndefined;
            parameter->GetType(type);

            const bool available = parameter->IsAvailable();
            const bool readable = available && parameter->IsReadable();
            const bool writable = available && parameter->IsWritable();
            std::string access = !available ? "n/a" : std::string(readable ? "R" : "") + (writable ? "W" : "");
            if (access.empty()) {
                access = "-";
            }

            std::string value = "-";
            if (readable && type != PvGenTypeCommand) {
                PvString value_pv;
                if (parameter->ToString(value_pv).IsOK()) {
                    value = pv(value_pv);
                }
            }

            std::ostringstream extra;
            if (readable && type == PvGenTypeInteger) {
                auto *node = dynamic_cast<PvGenInteger *>(parameter);
                std::int64_t min = 0, max = 0, inc = 0;
                if (node != nullptr && node->GetMin(min).IsOK() && node->GetMax(max).IsOK()) {
                    extra << " [" << min << ".." << max;
                    if (node->GetIncrement(inc).IsOK() && inc > 1) {
                        extra << ", inc " << inc;
                    }
                    extra << "]";
                }
            } else if (readable && type == PvGenTypeFloat) {
                auto *node = dynamic_cast<PvGenFloat *>(parameter);
                double min = 0.0, max = 0.0;
                if (node != nullptr && node->GetMin(min).IsOK() && node->GetMax(max).IsOK()) {
                    extra << " [" << min << ".." << max << "]";
                }
            } else if (type == PvGenTypeEnum) {
                auto *node = dynamic_cast<PvGenEnum *>(parameter);
                std::int64_t entries = 0;
                if (node != nullptr && node->GetEntriesCount(entries).IsOK() && entries > 0) {
                    extra << " {";
                    for (std::int64_t e = 0; e < entries; ++e) {
                        const PvGenEnumEntry *entry = nullptr;
                        if (node->GetEntryByIndex(e, &entry).IsOK() && entry != nullptr) {
                            PvString entry_name;
                            entry->GetName(entry_name);
                            extra << (e > 0 ? " " : "") << pv(entry_name);
                        }
                    }
                    extra << "}";
                }
            }

            os << pv(category_pv) << " | " << pv(name_pv) << " | " << typeName(type) << " | " << access
                    << " | " << value << extra.str() << "\n";
            ++dumped;
        }
        return dumped;
    }
} // namespace fx10
