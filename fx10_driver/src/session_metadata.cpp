#include "session_metadata.h"
#include "pixel_format.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace fx10 {
    namespace {
        void WriteAll(int fd, const std::string &text, const JsonlFile::WriteHook &hook = {}) {
            std::size_t done = 0;
            while (done < text.size()) {
                const auto n = hook ? hook(fd, text.data() + done, text.size() - done)
                                    : ::write(fd, text.data() + done, text.size() - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) throw MetadataError("metadata write: " + std::string(n == 0 ? "no progress" : std::strerror(errno)));
                done += static_cast<std::size_t>(n);
            }
        }
        void SyncDirectory(const std::filesystem::path &path) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0) throw MetadataError("open metadata directory: " + std::string(std::strerror(errno)));
            const int result = ::fsync(fd);
            const int saved = errno;
            ::close(fd);
            if (result != 0) throw MetadataError("sync metadata directory: " + std::string(std::strerror(saved)));
        }
        template <typename T> nlohmann::json Value(const std::optional<T> &v) {
            return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
        }
        nlohmann::json Temperature(const std::optional<double> &v) {
            const bool valid = v && std::isfinite(*v);
            return {{"value", valid ? nlohmann::json(*v) : nlohmann::json(nullptr)},
                    {"unit", "C"}, {"status", valid ? "ok" : "unavailable"}};
        }
    }

    nlohmann::json RecordingContract(const std::string &pixel_format) {
        const auto *pf = GetPixelFormatInfo(pixel_format);
        return {
            {"format", "fx10-recording-contract-v1"},
            {"input", "SDK image payload"}, {"input_pixel_format", pixel_format},
            {"output", "ENVI BIL camera DN; no host radiometric correction"},
            {"frame_axes", "SDK width = spatial samples; SDK height = image rows (spectral bands unless status line is enabled)"},
            {"frame_to_bil", "one SDK frame becomes one BIL spatial scan line"},
            {"packed_pixels_unpacked", pf ? nlohmann::json(pf->packed) : nlohmann::json(nullptr)},
            {"storage_bytes_per_pixel", pf ? nlohmann::json(pf->storage_bpp) : nlohmann::json(nullptr)},
            {"pixel_format_value_bits", !pf ? nlohmann::json(nullptr) : nlohmann::json(
                pixel_format == "Mono8" ? 8 : pixel_format.find("10") != std::string::npos ? 10 : 12)},
            {"value_bits_semantics", "encoding range implied by PixelFormat; not an independent sensor ADC-mode readback"},
            {"packing_rule", "Mono10Packed/Mono12Packed: legacy GVSP pixel pairs unpacked to uint16; other supported formats copied"},
            {"padding", "SDK-declared X/Y padding omitted; lengths retained in original line index"},
            {"sdk_payload_byte_reconstruction", false},
            {"omitted_bytes", "padding bytes and unused packed bits are not preserved"},
            {"unsupported_payloads", "non-image, chunk-bearing, incomplete or inconsistent layouts rejected; event retained in index"},
            {"device_timestamp", "unscaled SDK GetTimestamp"},
            {"device_timestamp_unit", "unverified"}, {"device_timestamp_epoch", "unverified"},
            {"host_timestamps", "CLOCK_REALTIME/CLOCK_MONOTONIC receive observations in ns, not exposure timestamps"},
            {"line_index_format", "fx10-line-index-v2"},
            {"trigger_association_verified", false}, {"leading_loss", "unknown"}
        };
    }

    void PublishMetadata(const std::filesystem::path &path, const nlohmann::json &document) {
        const std::string text = document.dump(2) + "\n";
        auto part = path;
        part += ".part";
        int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) throw MetadataError("create " + part.string() + ": " + std::strerror(errno));
        try {
            WriteAll(fd, text);
            if (::fdatasync(fd) != 0) throw MetadataError("sync " + part.string() + ": " + std::strerror(errno));
            const int result = ::close(fd);
            fd = -1;
            if (result != 0) throw MetadataError("close " + part.string() + ": " + std::strerror(errno));
            // Link publishes atomically without overwriting an existing snapshot.
            if (::link(part.c_str(), path.c_str()) != 0) throw MetadataError("publish " + path.string() + ": " + std::strerror(errno));
            if (::unlink(part.c_str()) != 0) throw MetadataError("remove " + part.string() + ": " + std::strerror(errno));
            SyncDirectory(path.parent_path());
        } catch (...) {
            if (fd >= 0) ::close(fd);
            ::unlink(part.c_str());
            throw;
        }
    }

    JsonlFile::~JsonlFile() { if (fd_ >= 0) ::close(fd_); }

    void JsonlFile::Open(const std::filesystem::path &path) {
        if (fd_ >= 0 || failed_) throw MetadataError("metadata stream already open or failed");
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd_ < 0) throw MetadataError("create " + path.string() + ": " + std::strerror(errno));
        committed_bytes_ = 0;
        try { SyncDirectory(path.parent_path()); }
        catch (...) { failed_ = true; ::close(fd_); fd_ = -1; throw; }
    }

    void JsonlFile::Append(const nlohmann::json &row) {
        if (fd_ < 0 || failed_) throw MetadataError("metadata stream closed or failed");
        const std::string text = row.dump() + "\n";
        try {
            WriteAll(fd_, text, write_hook_);
        } catch (...) {
            failed_ = true;
            // Preserve a parseable prefix where truncation is still possible.
            if (::ftruncate(fd_, static_cast<off_t>(committed_bytes_)) != 0)
                throw MetadataError("metadata append and partial-row rollback failed: " + std::string(std::strerror(errno)));
            throw;
        }
        committed_bytes_ += text.size();
        if (::fdatasync(fd_) != 0) {
            failed_ = true;
            throw MetadataError("sync metadata row: " + std::string(std::strerror(errno)));
        }
    }

    void JsonlFile::Close() {
        if (fd_ < 0) return;
        const int fd = fd_;
        fd_ = -1;
        const int result = ::fdatasync(fd);
        const int saved = errno;
        const int closed = ::close(fd);
        if (result != 0 || closed != 0) {
            failed_ = true;
            throw MetadataError("close metadata stream: " + std::string(std::strerror(result != 0 ? saved : errno)));
        }
    }

    std::optional<std::int64_t> MissedTriggerDelta(const DeviceTelemetry &s) {
        if (!s.counter_binding_verified || !s.missed_raw || !s.missed_baseline || s.counter_regressed ||
            *s.missed_baseline < 0 || *s.missed_raw < *s.missed_baseline) return std::nullopt;
        return *s.missed_raw - *s.missed_baseline;
    }

    nlohmann::json BuildTelemetryJson(const DeviceTelemetry &s, const std::string &phase) {
        return {{"format", "fx10-telemetry-v1"}, {"phase", phase},
                {"hrt", s.host_realtime_ns}, {"hmn", s.host_monotonic_ns},
                {"read_finished_monotonic_ns", s.read_finished_monotonic_ns},
                {"temp", {{"proc_pcb", Temperature(s.temp_pcb_c)}, {"fpga", Temperature(s.temp_fpga_c)}}},
                {"missed_triggers", {{"node", "Counter1_Value"}, {"raw", Value(s.missed_raw)},
                    {"baseline", Value(s.missed_baseline)}, {"delta", Value(MissedTriggerDelta(s))},
                    {"read_status", s.missed_raw ? "ok" : "unavailable"},
                    {"counter_regressed", s.counter_regressed},
                    {"binding_verified_at_start", s.counter_binding_verified},
                    {"semantics", "raw Counter1_Value; missed-trigger meaning requires verified binding; not total triggers; reset/wrap uncorrected"}}}};
    }
}
