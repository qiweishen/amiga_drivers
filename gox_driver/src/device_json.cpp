#include "device_json.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

#include "string_util.h"

namespace gox {
    namespace {
        using nlohmann::ordered_json;

        bool ParseDoubleStrict(const std::string &s, double &out) {
            if (s.empty()) {
                return false;
            }
            errno = 0;
            char *end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            if (errno != 0 || end == s.c_str() || *end != '\0') {
                return false;
            }
            out = v;
            return true;
        }


        bool ParseInt64Strict(const std::string &s, int64_t &out) {
            if (s.empty()) {
                return false;
            }
            errno = 0;
            char *end = nullptr;
            const long long v = std::strtoll(s.c_str(), &end, 10);
            if (errno != 0 || end == s.c_str() || *end != '\0') {
                return false;
            }
            out = static_cast<int64_t>(v);
            return true;
        }


        // Transport values are numbers on the camera and should stay numbers in
        // the JSON; identity values (a serial like "00123") never get coerced.
        ordered_json CoerceNumber(const std::string &text) {
            int64_t i = 0;
            if (ParseInt64Strict(text, i)) {
                return i;
            }
            double d = 0;
            if (ParseDoubleStrict(text, d)) {
                return d;
            }
            return text;
        }


        ordered_json OptDouble(const std::optional<double> &v) {
            return v ? ordered_json(*v) : ordered_json(nullptr);
        }


        ordered_json OptInt(const std::optional<int64_t> &v) {
            return v ? ordered_json(*v) : ordered_json(nullptr);
        }


        ordered_json OptString(const std::optional<std::string> &v) {
            return v ? ordered_json(*v) : ordered_json(nullptr);
        }
    } // namespace


    bool Counter0Bound(const std::vector<AppliedFeature> &applied) {
        for (const AppliedFeature &a: applied) {
            if (a.name == "CounterEventSource") {
                return a.readback != "<absent>" && a.readback != "<failed>";
            }
        }
        return false;
    }


    const char *AuditOutcomeName(AuditOutcome outcome) {
        switch (outcome) {
            case AuditOutcome::kMatch:
                return "match";
            case AuditOutcome::kDeviates:
                return "deviates";
            case AuditOutcome::kRecorded:
                return "recorded";
            case AuditOutcome::kUnreadable:
                break;
        }
        return "unreadable";
    }


    // Every feature below is one the driver deliberately never writes: the
    // factory load (UserSetLoad[Default]) is what puts it at the documented
    // value, and this table is the evidence that it did. Manual page numbers in
    // the comments; expected "" means "record the value, do not judge it".
    const std::vector<RawAuditExpectation> &RawAuditExpectations() {
        static const std::vector<RawAuditExpectation> table = {
            // Video processing chain (p.47 lists exactly what a bypass would skip)
            {"VideoProcessBypassMode", "Off"},
            // Analog / black level (p.148). The factory BlackLevelSelector is
            // All, so a plain read is BlackLevel[All]; Red/Blue need the
            // selector table below.
            {"BlackLevel", "0"},
            {"BlackLevelSelector", ""},
            {"GainSelector", ""},
            {"GainAuto", "Off"},
            {"ExposureAuto", "Off"},
            {"BalanceWhiteAuto", "Off"},
            // Tone / colour (p.152-153, p.172: LUT off means gamma 1.0, so the
            // Gamma register's 0.45 is inert and only recorded)
            {"LUTMode", "Off"},
            {"Gamma", ""},
            {"ColorTransformationRGBMode", "Off"},
            // Enhancers: EdgeEnhancer is mono-only and ColorEnhancer is
            // RGB-format-only (p.110), so "<absent>" is the normal reading on a
            // colour model streaming Bayer.
            {"EdgeEnhancerEnable", "Off|false|0"},
            {"EdgeEnhancerLevel", ""},
            {"ColorEnhancerEnable", "Off|false|0"},
            {"ShadingMode", "Off"},
            {"ShadingCorrectionMode", ""},
            // Test/overlay sources (p.158)
            {"TestPattern", "Off"},
            {"OverlayMode", "Off|false|0"},
            // Compression: forced Off under a 12-bit format anyway (p.92)
            {"ImageCompressionMode", "Off"},
            // Gradation compression: the mode flag and the curve it would apply
            // persist independently (p.137-138, gains reach -66 dB)
            {"GradationCompressionMode", "Off"},
            {"GradationCompression1stKneePoint", "50"},
            {"GradationCompression1stGain", "0"},
            {"GradationCompression2ndKneePoint", "100"},
            {"GradationCompression2ndGain", "0"},
            // Geometry-changing readout (p.134). Sum vs Average changes the code
            // values by the bin factor, so the arithmetic is recorded too.
            {"ImageScalingMode", "Off"},
            {"ImageScalingSumMode", ""},
            {"ImageScalingHorizontal", "1"},
            {"ImageScalingVertical", "1"},
            {"DecimationHorizontal", "1"},
            {"DecimationVertical", "1"},
            {"BinningHorizontal", "1"},
            {"BinningVertical", "1"},
            {"BinningHorizontalMode", ""},
            {"BinningVerticalMode", ""},
            {"ReverseX", "Off|false|0"},
            {"ReverseY", "Off|false|0"},
            {"ROICentered", "Off|false|0"},
            {"MultiRoiMode", "Off|false|0"},
            {"SequencerMode", "Off"},
            // Exposure/acquisition modes: ExposureTime is only the commanded
            // quantity under Timed (p.38/p.171), so the mode is evidence.
            {"ExposureMode", "Timed"},
            {"ExposureModeOption", "Off"},
            {"AcquisitionMode", "Continuous"},
            // Chunk stays off: jai-raw-seg v1 has no chunk tail (p.164)
            {"ChunkModeActive", "Off|false|0"},
            // Operator/plan state, recorded here so one file answers "how raw is
            // this dataset" without cross-referencing applied[].
            {"PixelFormat", ""},
            {"SensorDigitizationBits", ""},
            {"BlemishEnable", ""},
        };
        return table;
    }


    const std::vector<SelectorAuditExpectation> &SelectorAuditExpectations() {
        // Manual p.148: GainSelector {0: Analog All, 1: Digital Red, 2: Digital
        // Blue}, BlackLevelSelector {0: All, 1: Red, 3: Blue}. Digital gains are
        // magnifications (factory 1.0), the colour black levels are +-64 LSB
        // offsets (factory 0).
        static const std::vector<SelectorAuditExpectation> table = {
            {"GainSelector", "1", "Gain", "1", "Gain[DigitalRed]"},
            {"GainSelector", "2", "Gain", "1", "Gain[DigitalBlue]"},
            {"BlackLevelSelector", "1", "BlackLevel", "0", "BlackLevel[Red]"},
            {"BlackLevelSelector", "3", "BlackLevel", "0", "BlackLevel[Blue]"},
        };
        return table;
    }


    const std::vector<std::pair<const char *, const char *> > &SelectorHomeEntries() {
        // The entries the factory load leaves the selectors on (p.148).
        static const std::vector<std::pair<const char *, const char *> > table = {
            {"GainSelector", "0"}, // Analog All
            {"BlackLevelSelector", "0"}, // All
        };
        return table;
    }


    const std::vector<const char *> &IdentityFeatures() {
        // DeviceControl p.124-125, TransportLayerControl p.127,
        // ImageFormatControl p.131-132 (SensorWidth/Height prove the full array
        // next to the Width/Height actually streamed).
        static const std::vector<const char *> table = {
            "DeviceVendorName",
            "DeviceModelName",
            "DeviceSerialNumber",
            "DeviceVersion",
            "DeviceFirmwareVersion",
            "DeviceFpgaVersion",
            "DeviceUserID",
            "DeviceSFNCVersionMajor",
            "DeviceSFNCVersionMinor",
            "DeviceSFNCVersionSubMinor",
            "DeviceLinkSpeed",
            "GevMACAddress",
            "GevCurrentIPAddress",
            "SensorWidth",
            "SensorHeight",
            "WidthMax",
            "HeightMax",
            "Width",
            "Height",
            "OffsetX",
            "OffsetY",
            // Non-zero here means the factory blemish table is enumerable
            // through BlemishCompensationIndex/PositionX/Y (p.157), which
            // decides whether disabling blemish correction loses information.
            "BlemishCompensationNumber",
        };
        return table;
    }


    const std::vector<const char *> &TransportFeatures() {
        static const std::vector<const char *> table = {
            "PayloadSize",
            "GevSCPSPacketSize",
            "GevSCPD",
            "GevGVSPExtendedIDMode",
            "NetworkThroughputSafetyMargin",
            "AcquisitionFrameRate",
        };
        return table;
    }


    bool RawAuditMatches(const std::string &value, const std::string &expected) {
        const std::string v = common::StringUtil::Trim(value);
        double vn = 0;
        const bool v_numeric = ParseDoubleStrict(v, vn);
        for (const std::string &alt: common::StringUtil::Split(expected, '|')) {
            const std::string e = common::StringUtil::Trim(alt);
            if (common::StringUtil::EqualsCi(v, e)) {
                return true;
            }
            double en = 0;
            if (v_numeric && ParseDoubleStrict(e, en) && std::fabs(vn - en) < 1e-9) {
                return true;
            }
        }
        return false;
    }


    AuditOutcome ClassifyAudit(const std::string &value, const std::string &expected, bool readable) {
        if (!readable) {
            return AuditOutcome::kUnreadable;
        }
        if (expected.empty()) {
            return AuditOutcome::kRecorded; // read, but there is nothing to judge it against
        }
        return RawAuditMatches(value, expected) ? AuditOutcome::kMatch : AuditOutcome::kDeviates;
    }


    ordered_json BuildDeviceJson(const DeviceReport &r) {
        ordered_json doc;
        doc["format"] = "gox-device-json/1";
        doc["camera_id"] = r.camera_id;
        doc["ip"] = r.ip;
        doc["connection_path"] = r.connection_path;
        doc["created_realtime_ns"] = r.created_realtime_ns;

        ordered_json fl;
        fl["loaded"] = r.factory_load.loaded;
        fl["selector_entry"] = r.factory_load.selector_entry;
        fl["selector_readback"] = r.factory_load.selector_readback;
        fl["elapsed_ms"] = r.factory_load.elapsed_ms;
        fl["is_done_supported"] = r.factory_load.is_done_supported;
        doc["factory_load"] = std::move(fl);

        ordered_json identity = ordered_json::object();
        for (const auto &kv: r.identity) {
            identity[kv.first] = kv.second; // never coerced: "00123" stays a string
        }
        doc["identity"] = std::move(identity);

        ordered_json applied = ordered_json::array();
        for (const AppliedFeature &a: r.applied) {
            ordered_json e;
            e["name"] = a.name;
            e["requested"] = a.requested;
            e["readback"] = a.readback;
            e["strict"] = a.strict;
            e["tolerance_rel"] = a.tolerance_rel;
            applied.push_back(std::move(e));
        }
        doc["applied"] = std::move(applied); // array: order matters, names may repeat

        ordered_json audit = ordered_json::object();
        for (const AuditEntry &a: r.raw_audit) {
            ordered_json e;
            e["value"] = a.value;
            if (!a.expected.empty()) {
                e["expected"] = a.expected;
            }
            e["outcome"] = AuditOutcomeName(a.outcome);
            audit[a.name] = std::move(e);
        }
        doc["raw_audit"] = std::move(audit);

        ordered_json transport = ordered_json::object();
        for (const auto &kv: r.transport) {
            transport[kv.first] = CoerceNumber(kv.second);
        }
        transport["AcquisitionFrameRateMin"] = OptDouble(r.frame_rate_min_hz);
        transport["AcquisitionFrameRateMax"] = OptDouble(r.frame_rate_max_hz);
        doc["transport"] = std::move(transport);

        ordered_json derived;
        derived["exposure_time_us"] = OptDouble(r.exposure_time_us);
        derived["exposure_time_min_us"] = OptDouble(r.exposure_time_min_us);
        derived["exposure_time_max_us"] = OptDouble(r.exposure_time_max_us);
        derived["exposure_offset_us"] = kExposureOffsetUs;
        derived["actual_integration_us"] =
                r.exposure_time_us ? ordered_json(*r.exposure_time_us + kExposureOffsetUs) : ordered_json(nullptr);
        ordered_json pedestal;
        pedestal["black_level_register"] = OptString(r.black_level_register);
        pedestal["documented_pedestal"] = kDocumentedPedestal;
        pedestal["adjustment_resolution"] = kPedestalAdjustmentResolution;
        pedestal["pedestal_lsb8"] = kBlackLevelPedestalLsb8;
        pedestal["pedestal_lsb12_derived"] = kBlackLevelPedestalLsb12;
        // The camera reports no pedestal register: the 12-bit figure is this
        // driver's 8x16 rescaling of a specification value.
        pedestal["derived_by_driver"] = true;
        derived["black_level"] = std::move(pedestal);
        derived["sensor_digitization_bits"] = OptInt(r.sensor_digitization_bits);
        derived["thermal_limit_c"] = kThermalLimitC;
        doc["derived"] = std::move(derived);

        ordered_json runtime;
        runtime["buffer_count"] = r.runtime.buffer_count;
        runtime["queue_frames"] = r.runtime.queue_frames;
        runtime["socket_rx_requested_bytes"] = r.runtime.socket_rx_requested_bytes;
        runtime["socket_rx_effective_bytes"] = r.runtime.socket_rx_effective_bytes;
        runtime["socket_rx_set_result"] = r.runtime.socket_rx_set_result;
        runtime["socket_rx_read_result"] = r.runtime.socket_rx_read_result;
        runtime["socket_rx_effective_bytes_semantics"] =
                "Raw SDK SO_RCVBUF readback; valid only when socket_rx_read_result is OK. "
                "On Linux it may include doubled bookkeeping allocation; it is not payload capacity.";
        runtime["packet_size"] = r.runtime.packet_size;
        runtime["payload_size"] = r.runtime.payload_size;
        runtime["counter0_bound"] = r.runtime.counter0_bound;
        runtime["selector_restore_failed"] = r.runtime.selector_restore_failed;
        doc["runtime"] = std::move(runtime);

        ordered_json ptp;
        ptp["enabled"] = r.ptp.enabled;
        ptp["feature_found"] = r.ptp.feature_found;
        ptp["written"] = r.ptp.written;
        ptp["synchronized"] = r.ptp.synchronized;
        ptp["status"] = r.ptp.status;
        ptp["accuracy"] = r.ptp.accuracy >= 0 ? ordered_json(r.ptp.accuracy) : ordered_json(nullptr);
        ptp["lock_wait_ms"] = r.ptp.lock_wait_ms;
        ptp["raw_offset_ns"] = r.ptp.have_offset ? ordered_json(r.ptp.raw_offset_ns) : ordered_json(nullptr);
        ptp["adjusted_offset_ns"] = r.ptp.have_offset ? ordered_json(r.ptp.adjusted_offset_ns) : ordered_json(nullptr);
        ptp["drift_ns"] = r.ptp.have_offset ? ordered_json(r.ptp.drift_ns) : ordered_json(nullptr);
        ptp["tai_utc_offset_detected"] = r.ptp.tai_detected;
        ptp["assumed_tai_utc_offset_s"] = kAssumedTaiUtcOffsetS;
        // The manual documents neither the grandmaster's timescale nor a leap
        // second count; 37 s is this driver's assumption (p.121).
        ptp["driver_assumption"] = true;
        doc["ptp"] = std::move(ptp);

        doc["recording_contract"] = {
            {"payload", "unmodified GetAcquiredSize bytes from SDK buffer"},
            {"device_timestamp", "unscaled SDK GetTimestamp; legacy field device_ts_ns/dts"},
            {"device_timestamp_unit", "unverified"}, {"device_timestamp_epoch", "unverified"},
            {"frame_layout", "padding/chunk count/payload type/operation result in original idx.jsonl"},
            {"index_recovery_limit", "these supplementary fields cannot be rebuilt from the version-1 raw header"}
        };

        return doc;
    }


    std::string BuildTelemetryLine(const TelemetrySample &s) {
        ordered_json row;
        row["hrt"] = s.hrt;
        ordered_json temp;
        temp["sensor"] = OptDouble(s.temp_sensor);
        temp["mainboard"] = OptDouble(s.temp_mainboard);
        temp["fpga"] = OptDouble(s.temp_fpga);
        row["temp"] = std::move(temp);
        row["trig"] = OptInt(s.trig);
        row["trig_overflow"] = s.trig_overflow;
        row["pause_rx"] = OptInt(s.pause_rx);
        ordered_json ptp;
        ptp["status"] = OptString(s.ptp_status);
        ptp["accuracy"] = OptInt(s.ptp_accuracy);
        ptp["offset_ns"] = OptInt(s.ptp_offset_ns);
        row["ptp"] = std::move(ptp);
        return row.dump();
    }
} // namespace gox
