#include "scan_h5_file.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <limits>
#include <unistd.h>

#include <hdf5.h>

#include "logger.h"


namespace lms4xxx {
    namespace {
        common::DriverLog g_log{"LMS4xxx"};

        // 3: adds the /telemetry group, the device-audit root attributes and the
        // closed_cleanly/frames_total completeness marker (docs/FORMAT_H5.md).
        constexpr std::uint32_t kFormatVersion = 3;

        // Chunk of the 1-D metadata columns
        constexpr hsize_t kMetaChunkFrames = 4096;


        // libhdf5 is not thread-safe: one process-wide lock for all instances
        std::mutex &LibraryMutex() {
            static std::mutex mutex;
            return mutex;
        }


        // Under the lock. Silences libhdf5's stderr error stack (errors go to LastError())
        void EnsureLibraryReady() {
            static bool ready = false;
            if (!ready) {
                H5open();
                H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
                ready = true;
            }
        }


        // RAII hid_t
        class Handle {
        public:
            Handle() = default;

            Handle(hid_t id, herr_t (*closer)(hid_t)) : id_(id), closer_(closer) {
            }

            ~Handle() { Reset(); }

            Handle(const Handle &) = delete;

            Handle &operator=(const Handle &) = delete;

            Handle(Handle &&other) noexcept : id_(other.id_), closer_(other.closer_) {
                other.id_ = H5I_INVALID_HID;
            }

            Handle &operator=(Handle &&other) noexcept {
                if (this != &other) {
                    Reset();
                    id_ = other.id_;
                    closer_ = other.closer_;
                    other.id_ = H5I_INVALID_HID;
                }
                return *this;
            }

            [[nodiscard]] bool Valid() const { return id_ >= 0; }

            [[nodiscard]] hid_t Get() const { return id_; }

            herr_t Close() {
                const hid_t id = id_;
                id_ = H5I_INVALID_HID;
                return id >= 0 && closer_ != nullptr ? closer_(id) : 0;
            }

            void Reset() {
                Close();
            }

        private:
            hid_t id_ = H5I_INVALID_HID;
            herr_t (*closer_)(hid_t) = nullptr;
        };


        enum class ColType : std::uint8_t { kU8, kU16, kU32, kI32, kI64, kF32, kStr16 };


        // One /frames/<name> dataset per FrameMeta field
        struct ColumnDesc {
            const char *name;
            ColType type;
            std::size_t offset;
            std::size_t size;
            const char *unit; ///< nullptr = no unit attribute
            const char *description;
        };

#define LMS_COL(field, type, unit, desc) \
    ColumnDesc { #field, type, offsetof(FrameMeta, field), sizeof(FrameMeta::field), unit, desc }

        constexpr ColumnDesc kFrameColumns[] = {
            LMS_COL(device_time_unix_us, ColType::kI64, "us",
                    "Telegram timestamp as microseconds since the Unix epoch (NTP-synchronised device clock); "
                    "0 when NTP is off, the time stamp block is then not output (has_timestamp == 0)."),
            LMS_COL(time_since_startup_us, ColType::kU32, "us",
                    "Device uptime at the start of the scan, microseconds (32-bit wrap)."),
            LMS_COL(transmission_time_us, ColType::kU32, "us",
                    "Device uptime when the telegram was transmitted, microseconds (32-bit wrap)."),
            LMS_COL(telegram_counter, ColType::kU16, nullptr, "Telegram counter, increments per telegram (16-bit wrap)."),
            LMS_COL(scan_counter, ColType::kU16, nullptr, "Scan counter, increments per scan (16-bit wrap)."),
            LMS_COL(num_points, ColType::kU16, nullptr,
                    "Valid points in this scan; /channels/* rows are zero-padded beyond this index."),
            LMS_COL(start_angle, ColType::kI32, "1e-4 deg", "Angle of point 0 of this scan."),
            LMS_COL(angle_step, ColType::kU16, "1e-4 deg",
                    "Angular step between consecutive points: angle_deg[i] = (start_angle + i * angle_step) / 1e4."),
            LMS_COL(scan_frequency, ColType::kU32, "1/100 Hz", "Scan frequency (60000 = 600 Hz)."),
            LMS_COL(measurement_frequency, ColType::kU32, "100 Hz",
                    "Inverse of the time between two measurement shots, in units of 100 Hz."),
            LMS_COL(device_version, ColType::kU16, nullptr, "Device version number."),
            LMS_COL(device_number, ColType::kU16, nullptr, "Device number."),
            LMS_COL(serial_number, ColType::kU32, nullptr, "Device serial number (YYWW format)."),
            LMS_COL(device_status_1, ColType::kU8, nullptr, "Device status word 1: 0 = ok, 1 = error."),
            LMS_COL(device_status_2, ColType::kU8, nullptr, "Device status word 2: 0 = ok, 1 = error."),
            LMS_COL(digital_input_1, ColType::kU8, nullptr, "Digital input state 1."),
            LMS_COL(digital_input_2, ColType::kU8, nullptr, "Digital input state 2."),
            LMS_COL(digital_output_1, ColType::kU8, nullptr, "Digital output state 1."),
            LMS_COL(digital_output_2, ColType::kU8, nullptr, "Digital output state 2."),
            LMS_COL(has_encoder, ColType::kU8, nullptr, "1 when the telegram carries an encoder block."),
            LMS_COL(encoder_position, ColType::kU32, "ticks", "Encoder position; valid when has_encoder == 1."),
            LMS_COL(has_timestamp, ColType::kU8, nullptr, "1 when the telegram carries a timestamp block."),
            LMS_COL(ts_year, ColType::kU16, nullptr, "Telegram timestamp year (UTC); see device_time_unix_us."),
            LMS_COL(ts_month, ColType::kU8, nullptr, "Telegram timestamp month 1..12."),
            LMS_COL(ts_day, ColType::kU8, nullptr, "Telegram timestamp day 1..31."),
            LMS_COL(ts_hour, ColType::kU8, nullptr, "Telegram timestamp hour 0..23."),
            LMS_COL(ts_minute, ColType::kU8, nullptr, "Telegram timestamp minute 0..59."),
            LMS_COL(ts_second, ColType::kU8, nullptr, "Telegram timestamp second 0..59."),
            LMS_COL(ts_microsecond, ColType::kU32, "us", "Telegram timestamp microseconds 0..999999."),
            LMS_COL(y_rotation, ColType::kF32, "deg", "Reserved position field (Y rotation) from the telegram."),
            LMS_COL(has_device_name, ColType::kU8, nullptr, "1 when the telegram carries a device name."),
            LMS_COL(device_name, ColType::kStr16, nullptr,
                    "Device name from the telegram (NUL-terminated, at most 15 characters)."),
        };

#undef LMS_COL


        // Guard: ColType width must match the FrameMeta field
        constexpr std::size_t ColWidth(ColType type) {
            switch (type) {
                case ColType::kU8:
                    return 1;
                case ColType::kU16:
                    return 2;
                case ColType::kU32:
                    return 4;
                case ColType::kI32:
                    return 4;
                case ColType::kI64:
                    return 8;
                case ColType::kF32:
                    return 4;
                case ColType::kStr16:
                    return 16;
            }
            return 0;
        }

        constexpr bool ColumnsConsistent() {
            for (const auto &column: kFrameColumns) {
                if (ColWidth(column.type) != column.size) {
                    return false;
                }
            }
            return true;
        }

        static_assert(ColumnsConsistent(), "kFrameColumns: a ColType tag does not match its FrameMeta field width");


        hid_t MemType(ColType type) {
            switch (type) {
                case ColType::kU8:
                    return H5T_NATIVE_UINT8;
                case ColType::kU16:
                    return H5T_NATIVE_UINT16;
                case ColType::kU32:
                    return H5T_NATIVE_UINT32;
                case ColType::kI32:
                    return H5T_NATIVE_INT32;
                case ColType::kI64:
                    return H5T_NATIVE_INT64;
                case ColType::kF32:
                    return H5T_NATIVE_FLOAT;
                default:
                    return H5I_INVALID_HID; // strings use the file's fixed string type
            }
        }


        // Explicit little-endian file types
        hid_t FileType(ColType type) {
            switch (type) {
                case ColType::kU8:
                    return H5T_STD_U8LE;
                case ColType::kU16:
                    return H5T_STD_U16LE;
                case ColType::kU32:
                    return H5T_STD_U32LE;
                case ColType::kI32:
                    return H5T_STD_I32LE;
                case ColType::kI64:
                    return H5T_STD_I64LE;
                case ColType::kF32:
                    return H5T_IEEE_F32LE;
                default:
                    return H5I_INVALID_HID;
            }
        }


        // Variable-length UTF-8 (h5py reads it as str)
        bool WriteStringAttr(hid_t loc, const char *name, const std::string &value) {
            Handle type(H5Tcopy(H5T_C_S1), H5Tclose);
            if (!type.Valid() || H5Tset_size(type.Get(), H5T_VARIABLE) < 0 ||
                H5Tset_cset(type.Get(), H5T_CSET_UTF8) < 0) {
                return false;
            }
            Handle space(H5Screate(H5S_SCALAR), H5Sclose);
            if (!space.Valid()) {
                return false;
            }
            Handle attr(H5Acreate2(loc, name, type.Get(), space.Get(), H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
            if (!attr.Valid()) {
                return false;
            }
            const char *text = value.c_str();
            return H5Awrite(attr.Get(), type.Get(), &text) >= 0;
        }


        template<typename T>
        bool WriteScalarAttr(hid_t loc, const char *name, hid_t file_type, hid_t mem_type, const T &value) {
            Handle space(H5Screate(H5S_SCALAR), H5Sclose);
            if (!space.Valid()) {
                return false;
            }
            Handle attr(H5Acreate2(loc, name, file_type, space.Get(), H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
            if (!attr.Valid()) {
                return false;
            }
            return H5Awrite(attr.Get(), mem_type, &value) >= 0;
        }
    } // namespace


    struct ScanH5File::Impl {
        struct Column {
            Handle dset;
            ColType type;
            std::size_t offset;
            std::size_t size;
        };

        struct Channel {
            Handle dset;
            std::size_t record_offset;
            std::size_t elem_size;
        };

        Options opt;
        std::string last_error;
        bool failed = false;

        // Destruction order: datasets, groups, file
        Handle file;
        Handle frames_group;
        Handle channels_group;
        Handle str16; // fixed 16-byte NUL-terminated string (device_name)
        std::vector<Column> columns;
        std::vector<Channel> channels;

        std::vector<std::uint8_t> gather; // column staging buffer
        hsize_t frames = 0;
        bool open = false;
        bool compression_active = false;

        // /telemetry: device self-report, one row per poll
        Handle telemetry_group;
        Handle telemetry_str; // variable-length UTF-8 for the warning text
        std::vector<Handle> telemetry_columns;
        hsize_t telemetry_rows = 0;

        bool Fail(std::string message) {
            if (!failed) {
                last_error = std::move(message);
            }
            failed = true;
            return false;
        }

        // Returns false when HDF5 could not finish the file. H5Fclose performs
        // the final metadata flush, so discarding its result reported an ENOSPC
        // truncation as a clean stop.
        bool CloseLocked() {
            bool ok = !failed;
            if (open && file.Valid()) {
                if (!FlushLocked()) {
                    ok = false;
                }
                // A failed batch may leave uneven datasets. Never advertise
                // it as complete. Readers also check lengths and the run result;
                // this marker cannot prove a later H5Fclose succeeded.
                if (!WriteScalarAttr(file.Get(), "frames_total", H5T_STD_U64LE, H5T_NATIVE_UINT64,
                                     static_cast<std::uint64_t>(frames)) ||
                    !WriteScalarAttr(file.Get(), "closed_cleanly", H5T_STD_U8LE, H5T_NATIVE_UINT8,
                                     static_cast<std::uint8_t>(failed ? 0 : 1))) {
                    // SWMR forbids adding attributes after the layout is frozen,
                    // so this is expected there and must not fail the close.
                    if (!opt.swmr) {
                        last_error = "cannot write the completeness marker";
                        ok = false;
                    }
                }
                if (!FlushLocked()) {
                    ok = false;
                }
            }
            telemetry_columns.clear();
            telemetry_str.Reset();
            telemetry_group.Reset();
            channels.clear();
            columns.clear();
            str16.Reset();
            channels_group.Reset();
            frames_group.Reset();
            if (file.Valid()) {
                if (file.Close() < 0) {
                    last_error = "H5Fclose failed on '" + opt.path + "' (the file may be truncated)";
                    ok = false;
                }
            }
            open = false;
            failed = failed || !ok;
            return ok;
        }

        bool OpenLocked(const Options &options) {
            if (open && !CloseLocked()) {
                return false;
            }
            opt = options;
            last_error.clear();
            failed = false;
            frames = 0;
            telemetry_rows = 0; // per FILE, not per run: each split starts empty
            fsync_unavailable_logged = false;
            compression_active = false;

            if (opt.chunk_frames == 0) {
                return Fail("chunk_frames must be >= 1");
            }
            if (opt.compression_level < 0 || opt.compression_level > 9) {
                return Fail("compression_level must be in [0, 9]");
            }
            if (opt.compression_level > 0) {
                compression_active = H5Zfilter_avail(H5Z_FILTER_DEFLATE) > 0;
            }

            // 1.10 file format: SWMR-capable, readable by any HDF5 >= 1.10
            Handle fapl(H5Pcreate(H5P_FILE_ACCESS), H5Pclose);
            if (!fapl.Valid() || H5Pset_libver_bounds(fapl.Get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST) < 0) {
                return Fail("cannot prepare the file access property list");
            }

            // EXCL, not TRUNC: the path already carries the session timestamp
            // and the split index, so an existing file means something is wrong
            // with the run layout — silently destroying a recording is never the
            // right answer to that.
            file = Handle(H5Fcreate(opt.path.c_str(), H5F_ACC_EXCL, H5P_DEFAULT, fapl.Get()), H5Fclose);
            if (!file.Valid()) {
                return Fail("cannot create '" + opt.path + "' (it may already exist)");
            }

            if (!CreateLayout()) {
                CloseLocked();
                return false;
            }

            // SWMR forbids creating objects afterwards and needs all attributes closed
            if (opt.swmr && H5Fstart_swmr_write(file.Get()) < 0) {
                const std::string error = "H5Fstart_swmr_write failed on '" + opt.path + "'";
                CloseLocked();
                return Fail(error);
            }

            open = true;
            return true;
        }

        bool CreateLayout() {
            frames_group = Handle(H5Gcreate2(file.Get(), "frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
            channels_group = Handle(H5Gcreate2(file.Get(), "channels", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                                    H5Gclose);
            if (!frames_group.Valid() || !channels_group.Valid()) {
                return Fail("cannot create the /frames and /channels groups");
            }

            str16 = Handle(H5Tcopy(H5T_C_S1), H5Tclose);
            if (!str16.Valid() || H5Tset_size(str16.Get(), sizeof(FrameMeta::device_name)) < 0 ||
                H5Tset_strpad(str16.Get(), H5T_STR_NULLTERM) < 0) {
                return Fail("cannot create the fixed string type");
            }

            if (!WriteRootAttributes()) {
                return false;
            }
            for (const auto &column: kFrameColumns) {
                if (!CreateColumn(column)) {
                    return false;
                }
            }
            for (const auto &spec: kChannelSpecs) {
                if ((opt.channel_mask & spec.bit) && !CreateChannel(spec)) {
                    return false;
                }
            }
            return CreateTelemetry();
        }

        bool WriteRootAttributes() {
            const hid_t f = file.Get();
            auto str = [&](const char *name, const std::string &value) {
                return WriteStringAttr(f, name, value) || Fail(std::string("cannot write attribute ") + name);
            };
            auto u8 = [&](const char *name, std::uint8_t value) {
                return WriteScalarAttr(f, name, H5T_STD_U8LE, H5T_NATIVE_UINT8, value) ||
                       Fail(std::string("cannot write attribute ") + name);
            };
            auto u16 = [&](const char *name, std::uint16_t value) {
                return WriteScalarAttr(f, name, H5T_STD_U16LE, H5T_NATIVE_UINT16, value) ||
                       Fail(std::string("cannot write attribute ") + name);
            };
            auto u32 = [&](const char *name, std::uint32_t value) {
                return WriteScalarAttr(f, name, H5T_STD_U32LE, H5T_NATIVE_UINT32, value) ||
                       Fail(std::string("cannot write attribute ") + name);
            };
            auto f64 = [&](const char *name, double value) {
                return WriteScalarAttr(f, name, H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, value) ||
                       Fail(std::string("cannot write attribute ") + name);
            };

            unsigned maj = 0, min = 0, rel = 0;
            H5get_libversion(&maj, &min, &rel);
            const std::string hdf5_version =
                    std::to_string(maj) + "." + std::to_string(min) + "." + std::to_string(rel);
            const std::string compression =
                    compression_active ? "gzip-" + std::to_string(opt.compression_level) : "none";

            const bool ok =
                    str("format", "lms4xxx-h5") &&
                    u32("format_version", kFormatVersion) &&
                    str("producer", "amiga_drivers lms4xxx_driver (SICK LMS4xxx CoLa B scan recorder)") &&
                    str("instance_id", opt.instance) &&
                    str("session_timestamp", opt.session_timestamp) &&
                    u32("split_index", opt.split_index) &&
                    str("hdf5_library_version", hdf5_version) &&
                    u16("channel_mask", opt.channel_mask) &&
                    str("channels", ChannelNames(opt.channel_mask)) &&
                    u16("points_per_scan", static_cast<std::uint16_t>(kMaxPointsPerScan)) &&
                    f64("config_start_angle_deg", opt.start_angle_deg) &&
                    f64("config_stop_angle_deg", opt.stop_angle_deg) &&
                    f64("config_angle_step_deg", opt.angle_step_deg) &&
                    u16("config_output_rate", opt.output_rate) &&
                    str("config_remission", opt.remission) &&
                    str("device_firmware", opt.device_firmware) &&
                    str("device_order_number", opt.device_order_number) &&
                    str("device_type", opt.device_type) &&
                    str("device_name", opt.device_name) &&
                    u8("device_keeps_flagged_points", opt.device_keeps_flagged_points ? 1 : 0) &&
                    WriteAuditAttributes() &&
                    str("compression", compression) &&
                    u8("swmr", opt.swmr ? 1 : 0) &&
                    str("layout",
                        "/frames/<field>: one 1-D dataset per scan-level field, row i = scan i. "
                        "/channels/<name>: 2-D [scan, point] raw device values; physical = raw * scale_factor + "
                        "scale_offset (attributes on each dataset), rows zero-padded beyond /frames/num_points[i]. "
                        "All datasets share the scan index. Split files of one run continue the same layout.");
            if (!ok) {
                return false;
            }
            if (!opt.config_yaml.empty() && !str("config_yaml", opt.config_yaml)) {
                return false;
            }

            if (!WriteStringAttr(frames_group.Get(), "description",
                                 "Per-scan metadata: one 1-D dataset per field, row i belongs to scan i of "
                                 "every /channels/* dataset.")) {
                return Fail("cannot write the /frames description");
            }
            if (!WriteStringAttr(channels_group.Get(), "description",
                                 "Raw per-point device values, one 2-D [scan, point] dataset per recorded channel.") ||
                !WriteStringAttr(channels_group.Get(), "angle_formula",
                                 "angle_deg[scan, i] = (frames/start_angle[scan] + i * frames/angle_step[scan]) / 1e4"
                                 " (+ channels/angl correction where recorded)") ||
                !WriteScalarAttr(channels_group.Get(), "points_per_scan", H5T_STD_U16LE, H5T_NATIVE_UINT16,
                                 static_cast<std::uint16_t>(kMaxPointsPerScan))) {
                return Fail("cannot write the /channels attributes");
            }
            return true;
        }

        // What the device said about itself at bring-up. Optional throughout: a
        // firmware that does not answer one of these reads simply leaves the
        // attribute out, and a reader must treat "absent" as "unknown", never
        // as a value.
        bool WriteAuditAttributes() {
            const hid_t f = file.Get();
            const auto &a = opt.audit;
            const auto f64 = [&](const char *name, double value) {
                return WriteScalarAttr(f, name, H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, value) ||
                       Fail(std::string("cannot write attribute ") + name);
            };
            const auto u32 = [&](const char *name, std::uint32_t value) {
                return WriteScalarAttr(f, name, H5T_STD_U32LE, H5T_NATIVE_UINT32, value) ||
                       Fail(std::string("cannot write attribute ") + name);
            };
            const auto i32 = [&](const char *name, std::int32_t value) {
                return WriteScalarAttr(f, name, H5T_STD_I32LE, H5T_NATIVE_INT32, value) ||
                       Fail(std::string("cannot write attribute ") + name);
            };

            if (a.temperature_c && !f64("device_temperature_c_at_start", *a.temperature_c)) return false;
            if (a.operating_hours_deci && !f64("device_operating_hours", *a.operating_hours_deci / 10.0)) return false;
            if (a.power_on_count && !u32("device_power_on_count", *a.power_on_count)) return false;
            if (a.device_state && !i32("device_state_at_start", *a.device_state)) return false;

            // LMPscancfg: the scan configuration BEFORE any filter, i.e. what the
            // device actually runs at, independent of what LMPoutputRange asked
            // the output stage to emit.
            if (a.scan_frequency_centi_hz && !f64("device_scan_frequency_hz", *a.scan_frequency_centi_hz / 100.0))
                return false;
            if (a.angular_resolution_1e4 && !f64("device_angular_resolution_deg", *a.angular_resolution_1e4 / 1e4))
                return false;
            if (a.start_angle_1e4 && !f64("device_start_angle_deg", *a.start_angle_1e4 / 1e4)) return false;
            if (a.stop_angle_1e4 && !f64("device_stop_angle_deg", *a.stop_angle_1e4 / 1e4)) return false;

            // 0 = free-running, which is what continuous output requires.
            if (a.laser_trigger_source && !i32("device_laser_trigger_source", *a.laser_trigger_source)) return false;
            if (a.laser_timeout_s && !i32("device_laser_timeout_s", *a.laser_timeout_s)) return false;

            // Never written by the driver; recorded because mSCloadappdef does
            // not reset Interfaces parameters, so a leftover role persists.
            if (a.motor_sync_role && !i32("device_motor_sync_role", *a.motor_sync_role)) return false;
            if (a.motor_sync_phase_deg && !f64("device_motor_sync_phase_deg", *a.motor_sync_phase_deg)) return false;
            return true;
        }


        // /telemetry: one row per device poll. Separate from /frames because it
        // advances on its own (seconds) cadence, not per scan.
        bool CreateTelemetry() {
            telemetry_group = Handle(H5Gcreate2(file.Get(), "telemetry", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                                     H5Gclose);
            if (!telemetry_group.Valid()) {
                return Fail("cannot create the /telemetry group");
            }
            const auto column = [&](const char *name, hid_t file_type, const char *description) {
                const hsize_t dims[1] = {0};
                const hsize_t maxdims[1] = {H5S_UNLIMITED};
                const hsize_t chunk[1] = {64};
                Handle space(H5Screate_simple(1, dims, maxdims), H5Sclose);
                Handle dcpl(H5Pcreate(H5P_DATASET_CREATE), H5Pclose);
                if (!space.Valid() || !dcpl.Valid() || H5Pset_chunk(dcpl.Get(), 1, chunk) < 0) {
                    return false;
                }
                Handle dset(H5Dcreate2(telemetry_group.Get(), name, file_type, space.Get(), H5P_DEFAULT, dcpl.Get(),
                                       H5P_DEFAULT), H5Dclose);
                if (!dset.Valid()) {
                    return false;
                }
                if (!WriteStringAttr(dset.Get(), "description", description)) {
                    return false;
                }
                telemetry_columns.push_back(std::move(dset));
                return true;
            };
            // Variable-length UTF-8 for the clear-text warnings.
            telemetry_str = Handle(H5Tcopy(H5T_C_S1), H5Tclose);
            if (!telemetry_str.Valid() || H5Tset_size(telemetry_str.Get(), H5T_VARIABLE) < 0 ||
                H5Tset_cset(telemetry_str.Get(), H5T_CSET_UTF8) < 0) {
                return Fail("cannot create the telemetry string type");
            }
            if (!column("unix_us", H5T_STD_U64LE, "CLOCK_REALTIME microseconds when the answer was decoded") ||
                !column("temperature_c", H5T_IEEE_F32LE,
                        "Internal device temperature, deg C (sRN OPcurtmpdev, manual p.130); NaN = not answered") ||
                !column("device_state", H5T_STD_I32LE,
                        "sRN SCdevicestate: 0 busy/logged-in, 1 ready, 2 error; -1 = not answered") ||
                !column("warning_count", H5T_STD_U32LE, "Number of active device warnings/errors") ||
                !column("warnings", telemetry_str.Get(),
                        "Active warnings/errors in clear text, '; '-separated (sRN EMActiveCustomerInfo, p.125)")) {
                return Fail("cannot create the /telemetry datasets");
            }
            return WriteStringAttr(telemetry_group.Get(), "description",
                                   "Device self-report sampled every telemetry.interval_s seconds; rows are "
                                   "independent of the scan index.")
                       ? true
                       : Fail("cannot write the /telemetry description");
        }


        bool AppendTelemetryLocked(const TelemetrySample &sample) {
            if (!open || telemetry_columns.size() != 5) {
                return false;
            }
            const hsize_t index = telemetry_rows;
            const hsize_t new_size[1] = {index + 1};
            const hsize_t offset[1] = {index};
            const hsize_t count[1] = {1};

            std::string joined;
            for (const auto &warning: sample.warnings) {
                joined += (joined.empty() ? "" : "; ") + warning;
            }
            const char *joined_ptr = joined.c_str();

            const std::uint64_t unix_us = sample.unix_us;
            const float temperature = sample.temperature_c
                                          ? static_cast<float>(*sample.temperature_c)
                                          : std::numeric_limits<float>::quiet_NaN();
            const std::int32_t state = sample.device_state ? *sample.device_state : -1;
            const std::uint32_t warning_count = static_cast<std::uint32_t>(sample.warnings.size());

            const void *values[5] = {&unix_us, &temperature, &state, &warning_count, &joined_ptr};
            const hid_t mem_types[5] = {
                H5T_NATIVE_UINT64, H5T_NATIVE_FLOAT, H5T_NATIVE_INT32, H5T_NATIVE_UINT32, telemetry_str.Get()
            };
            for (std::size_t i = 0; i < telemetry_columns.size(); ++i) {
                const hid_t dset = telemetry_columns[i].Get();
                if (H5Dset_extent(dset, new_size) < 0) {
                    return Fail("H5Dset_extent failed on a /telemetry dataset");
                }
                Handle space(H5Dget_space(dset), H5Sclose);
                Handle mem(H5Screate_simple(1, count, nullptr), H5Sclose);
                if (!space.Valid() || !mem.Valid() ||
                    H5Sselect_hyperslab(space.Get(), H5S_SELECT_SET, offset, nullptr, count, nullptr) < 0 ||
                    H5Dwrite(dset, mem_types[i], mem.Get(), space.Get(), H5P_DEFAULT, values[i]) < 0) {
                    return Fail("H5Dwrite failed on a /telemetry dataset");
                }
            }
            ++telemetry_rows;
            return true;
        }


        bool CreateColumn(const ColumnDesc &desc) {
            const hsize_t dims[1] = {0};
            const hsize_t maxdims[1] = {H5S_UNLIMITED};
            const hsize_t chunk[1] = {kMetaChunkFrames};

            Handle space(H5Screate_simple(1, dims, maxdims), H5Sclose);
            Handle dcpl(H5Pcreate(H5P_DATASET_CREATE), H5Pclose);
            if (!space.Valid() || !dcpl.Valid() || H5Pset_chunk(dcpl.Get(), 1, chunk) < 0) {
                return Fail(std::string("cannot prepare dataset /frames/") + desc.name);
            }

            const hid_t file_type = desc.type == ColType::kStr16 ? str16.Get() : FileType(desc.type);
            Handle dset(H5Dcreate2(frames_group.Get(), desc.name, file_type, space.Get(), H5P_DEFAULT, dcpl.Get(),
                                   H5P_DEFAULT),
                        H5Dclose);
            if (!dset.Valid()) {
                return Fail(std::string("cannot create dataset /frames/") + desc.name);
            }
            if ((desc.unit != nullptr && !WriteStringAttr(dset.Get(), "unit", desc.unit)) ||
                !WriteStringAttr(dset.Get(), "description", desc.description)) {
                return Fail(std::string("cannot write the attributes of /frames/") + desc.name);
            }

            columns.push_back(Column{std::move(dset), desc.type, desc.offset, desc.size});
            return true;
        }

        bool CreateChannel(const ChannelSpec &spec) {
            const hsize_t dims[2] = {0, kMaxPointsPerScan};
            const hsize_t maxdims[2] = {H5S_UNLIMITED, kMaxPointsPerScan};
            const hsize_t chunk[2] = {opt.chunk_frames, kMaxPointsPerScan};

            Handle space(H5Screate_simple(2, dims, maxdims), H5Sclose);
            Handle dcpl(H5Pcreate(H5P_DATASET_CREATE), H5Pclose);
            Handle dapl(H5Pcreate(H5P_DATASET_ACCESS), H5Pclose);
            if (!space.Valid() || !dcpl.Valid() || !dapl.Valid() || H5Pset_chunk(dcpl.Get(), 2, chunk) < 0) {
                return Fail(std::string("cannot prepare dataset /channels/") + spec.name);
            }
            if (compression_active &&
                (H5Pset_shuffle(dcpl.Get()) < 0 ||
                 H5Pset_deflate(dcpl.Get(), static_cast<unsigned>(opt.compression_level)) < 0)) {
                return Fail(std::string("cannot enable compression on /channels/") + spec.name);
            }
            // Several chunks of cache so partial batches merge in memory
            const std::size_t chunk_bytes = opt.chunk_frames * kMaxPointsPerScan * spec.elem_size;
            const std::size_t cache_bytes = std::max<std::size_t>(std::size_t{1} << 20, 4 * chunk_bytes);
            if (H5Pset_chunk_cache(dapl.Get(), H5D_CHUNK_CACHE_NSLOTS_DEFAULT, cache_bytes,
                                   H5D_CHUNK_CACHE_W0_DEFAULT) < 0) {
                return Fail(std::string("cannot configure the chunk cache of /channels/") + spec.name);
            }

            const hid_t file_type = spec.elem_size == 1 ? H5T_STD_U8LE : H5T_STD_U16LE;
            Handle dset(H5Dcreate2(channels_group.Get(), spec.name, file_type, space.Get(), H5P_DEFAULT, dcpl.Get(),
                                   dapl.Get()),
                        H5Dclose);
            if (!dset.Valid()) {
                return Fail(std::string("cannot create dataset /channels/") + spec.name);
            }

            const hid_t d = dset.Get();
            if (!WriteStringAttr(d, "content", spec.content) ||
                !WriteScalarAttr(d, "scale_factor", H5T_IEEE_F32LE, H5T_NATIVE_FLOAT, spec.scale_factor) ||
                !WriteScalarAttr(d, "scale_offset", H5T_IEEE_F32LE, H5T_NATIVE_FLOAT, spec.scale_offset) ||
                !WriteStringAttr(d, "unit", spec.unit) ||
                !WriteStringAttr(d, "description", spec.description) ||
                !WriteScalarAttr(d, "channel_mask_bit", H5T_STD_U16LE, H5T_NATIVE_UINT16, spec.bit)) {
                return Fail(std::string("cannot write the attributes of /channels/") + spec.name);
            }

            channels.push_back(Channel{std::move(dset), spec.record_offset, spec.elem_size});
            return true;
        }

        bool AppendLocked(const ScanRecord *records, std::size_t count) {
            if (failed) {
                return false;
            }
            if (!open) {
                return Fail("file is not open");
            }
            if (count == 0) {
                return true;
            }
            if (records == nullptr) {
                return Fail("null records for a nonempty batch");
            }

            const hsize_t old_rows = frames;
            const hsize_t new_rows = frames + count;

            // /frames/*: gather, extend, write
            {
                const hsize_t dims[1] = {new_rows};
                const hsize_t start[1] = {old_rows};
                const hsize_t sel_count[1] = {count};
                Handle memspace(H5Screate_simple(1, sel_count, nullptr), H5Sclose);
                if (!memspace.Valid()) {
                    return Fail("cannot create the column memory space");
                }
                for (auto &column: columns) {
                    if (H5Dset_extent(column.dset.Get(), dims) < 0) {
                        return Fail("H5Dset_extent failed on a /frames dataset");
                    }
                    Handle filespace(H5Dget_space(column.dset.Get()), H5Sclose);
                    if (!filespace.Valid() ||
                        H5Sselect_hyperslab(filespace.Get(), H5S_SELECT_SET, start, nullptr, sel_count, nullptr) <
                        0) {
                        return Fail("hyperslab selection failed on a /frames dataset");
                    }
                    gather.resize(count * column.size);
                    for (std::size_t i = 0; i < count; ++i) {
                        std::memcpy(gather.data() + i * column.size,
                                    reinterpret_cast<const std::uint8_t *>(&records[i].meta) + column.offset,
                                    column.size);
                    }
                    const hid_t mem_type = column.type == ColType::kStr16 ? str16.Get() : MemType(column.type);
                    if (H5Dwrite(column.dset.Get(), mem_type, memspace.Get(), filespace.Get(), H5P_DEFAULT,
                                 gather.data()) < 0) {
                        return Fail("H5Dwrite failed on a /frames dataset");
                    }
                }
            }

            // /channels/*: gather, extend, write
            {
                const hsize_t dims[2] = {new_rows, kMaxPointsPerScan};
                const hsize_t start[2] = {old_rows, 0};
                const hsize_t sel_count[2] = {count, kMaxPointsPerScan};
                Handle memspace(H5Screate_simple(2, sel_count, nullptr), H5Sclose);
                if (!memspace.Valid()) {
                    return Fail("cannot create the channel memory space");
                }
                for (auto &channel: channels) {
                    if (H5Dset_extent(channel.dset.Get(), dims) < 0) {
                        return Fail("H5Dset_extent failed on a /channels dataset");
                    }
                    Handle filespace(H5Dget_space(channel.dset.Get()), H5Sclose);
                    if (!filespace.Valid() ||
                        H5Sselect_hyperslab(filespace.Get(), H5S_SELECT_SET, start, nullptr, sel_count, nullptr) <
                        0) {
                        return Fail("hyperslab selection failed on a /channels dataset");
                    }
                    const std::size_t row_bytes = kMaxPointsPerScan * channel.elem_size;
                    gather.resize(count * row_bytes);
                    for (std::size_t i = 0; i < count; ++i) {
                        std::memcpy(gather.data() + i * row_bytes,
                                    reinterpret_cast<const std::uint8_t *>(&records[i]) + channel.record_offset,
                                    row_bytes);
                    }
                    const hid_t mem_type = channel.elem_size == 1 ? H5T_NATIVE_UINT8 : H5T_NATIVE_UINT16;
                    if (H5Dwrite(channel.dset.Get(), mem_type, memspace.Get(), filespace.Get(), H5P_DEFAULT,
                                 gather.data()) < 0) {
                        return Fail("H5Dwrite failed on a /channels dataset");
                    }
                }
            }

            frames = new_rows;
            return true;
        }


        // H5Fflush only hands the HDF5 cache to the OS page cache. Without the
        // fdatasync a power cut loses everything the kernel has not written back
        // (default writeback window: tens of seconds), which is far more than
        // the flush_interval_ms the configuration promises.
        bool FlushLocked() {
            if (!open || !file.Valid()) {
                return Fail("file is not open");
            }
            if (H5Fflush(file.Get(), H5F_SCOPE_LOCAL) < 0) {
                return Fail("H5Fflush failed on '" + opt.path + "'");
            }
            // The sec2 VFD hands back a pointer to its own file descriptor.
            int *fd = nullptr;
            if (H5Fget_vfd_handle(file.Get(), H5P_DEFAULT, reinterpret_cast<void **>(&fd)) < 0 || fd == nullptr) {
                // Not fatal — the data is in the page cache and the next flush
                // tries again — but a silent durability downgrade is worse than
                // a noisy one, so say it once per file.
                if (!fsync_unavailable_logged) {
                    fsync_unavailable_logged = true;
                    g_log.Warn("[Writer] '{}': no file descriptor from HDF5, flushes are not fsynced — a power "
                               "loss can cost more than flush_interval_ms of scans", opt.path);
                }
                return true;
            }
            if (*fd >= 0 && ::fdatasync(*fd) != 0) {
                return Fail("fdatasync failed on '" + opt.path + "'");
            }
            return true;
        }

        bool fsync_unavailable_logged = false;
    };


    ScanH5File::ScanH5File() : impl_(std::make_unique<Impl>()) {
    }


    ScanH5File::~ScanH5File() {
        std::lock_guard lock(LibraryMutex());
        (void) impl_->CloseLocked(); // a destructor has nowhere to report to
    }


    bool ScanH5File::Open(const Options &options) {
        std::lock_guard lock(LibraryMutex());
        EnsureLibraryReady();
        return impl_->OpenLocked(options);
    }


    bool ScanH5File::Append(const ScanRecord *records, std::size_t count) {
        std::lock_guard lock(LibraryMutex());
        return impl_->AppendLocked(records, count);
    }


    bool ScanH5File::AppendTelemetry(const TelemetrySample &sample) {
        std::lock_guard lock(LibraryMutex());
        return impl_->AppendTelemetryLocked(sample);
    }


    bool ScanH5File::Flush() {
        std::lock_guard lock(LibraryMutex());
        return impl_->FlushLocked();
    }


    bool ScanH5File::Close() {
        std::lock_guard lock(LibraryMutex());
        return impl_->CloseLocked();
    }


    bool ScanH5File::IsOpen() const {
        std::lock_guard lock(LibraryMutex());
        return impl_->open;
    }


    std::uint64_t ScanH5File::FramesWritten() const {
        std::lock_guard lock(LibraryMutex());
        return static_cast<std::uint64_t>(impl_->frames);
    }


    bool ScanH5File::CompressionActive() const {
        return impl_->compression_active;
    }


    std::string ScanH5File::LastError() const {
        std::lock_guard lock(LibraryMutex()); // the write thread may be assigning it
        return impl_->last_error;
    }
} // namespace lms4xxx
