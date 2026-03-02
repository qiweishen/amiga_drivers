#include "csv_converter.h"
#include "point_cloud_types.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>


void BinaryToCsvConverter::set_progress_callback(CsvProgressCallback callback) {
    progress_callback_ = std::move(callback);
}


void BinaryToCsvConverter::set_progress_interval(uint64_t interval) {
    progress_interval_ = interval;
}


std::string BinaryToCsvConverter::default_csv_path(const std::string &binary_path) {
    size_t dot_pos = binary_path.rfind('.');
    if (dot_pos != std::string::npos) {
        return binary_path.substr(0, dot_pos) + ".csv";
    }
    return binary_path + ".csv";
}


// Read exactly len bytes from fd. Returns false on EOF or error.
static bool read_exact(int fd, void *buf, size_t len) {
    auto *ptr = static_cast<uint8_t *>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::read(fd, ptr, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            return false; // EOF
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}


CsvConversionResult BinaryToCsvConverter::convert(const std::string &binary_path,
                                                  const std::string &csv_path) const {
    CsvConversionResult result;

    // Get total file size for progress reporting.
    struct stat st{};
    if (::stat(binary_path.c_str(), &st) != 0) {
        result.error_message = "Cannot stat " + binary_path + ": " + std::strerror(errno);
        return result;
    }
    uint64_t total_bytes = static_cast<uint64_t>(st.st_size);

    // Open binary input.
    int fd = ::open(binary_path.c_str(), O_RDONLY);
    if (fd < 0) {
        result.error_message = "Cannot open " + binary_path + ": " + std::strerror(errno);
        return result;
    }

    // Advise the kernel for sequential read-ahead.
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    // Open CSV output with a large stdio buffer (256 KB).
    FILE *csv = std::fopen(csv_path.c_str(), "w");
    if (!csv) {
        result.error_message = "Cannot open " + csv_path + ": " + std::strerror(errno);
        ::close(fd);
        return result;
    }

    static constexpr size_t kCsvBufferSize = 256 * 1024;
    auto csv_buffer = std::make_unique<char[]>(kCsvBufferSize);
    std::setvbuf(csv, csv_buffer.get(), _IOFBF, kCsvBufferSize);

    // Validate file header.
    std::array<char, 8> magic{};
    uint32_t version = 0;

    if (!read_exact(fd, magic.data(), magic.size())) {
        result.error_message = "Failed to read file magic from " + binary_path;
        std::fclose(csv);
        ::close(fd);
        return result;
    }

    if (magic != FileFormat::kMagic) {
        result.error_message = "Invalid magic in " + binary_path;
        std::fclose(csv);
        ::close(fd);
        return result;
    }

    if (!read_exact(fd, &version, sizeof(version))) {
        result.error_message = "Failed to read version from " + binary_path;
        std::fclose(csv);
        ::close(fd);
        return result;
    }

    if (version != FileFormat::kVersion) {
        result.error_message = "Unsupported version " + std::to_string(version) + " in " + binary_path;
        std::fclose(csv);
        ::close(fd);
        return result;
    }

    result.bytes_read = FileFormat::kHeaderSize;

    // Write CSV header.
    std::fprintf(csv, "frame_index,timestamp_ns,point_index,x,y,z,intensity\n");

    // Read frames with a reusable point buffer.
    uint64_t frame_index = 0;
    std::vector<PointXYZI> points;

    while (true) {
        uint64_t timestamp_ns = 0;
        uint32_t num_points = 0;

        if (!read_exact(fd, &timestamp_ns, sizeof(timestamp_ns))) {
            break; // Normal EOF at frame boundary.
        }
        if (!read_exact(fd, &num_points, sizeof(num_points))) {
            result.error_message = "Truncated frame header at frame " + std::to_string(frame_index);
            break;
        }

        result.bytes_read += FileFormat::kFrameHeaderSize;

        // Read point data.
        size_t point_data_size = num_points * sizeof(PointXYZI);
        points.resize(num_points);

        if (num_points > 0) {
            if (!read_exact(fd, points.data(), point_data_size)) {
                result.error_message = "Truncated point data at frame " + std::to_string(frame_index);
                break;
            }
            result.bytes_read += point_data_size;
        }

        // Write CSV rows.
        for (uint32_t i = 0; i < num_points; ++i) {
            const auto &pt = points[i];
            // Invalid echoes from the SICK scanner are zero-filled
            // (see sick_scan_api.h: "Invalid echos are filled with 0 values").
            // Write null placeholders for these points to distinguish them
            // from valid data in downstream processing.
            if (pt.x == 0.0f && pt.y == 0.0f && pt.z == 0.0f) {
                std::fprintf(csv, "%" PRIu64 ",%" PRIu64 ",%u,,,,\n",
                             frame_index,
                             timestamp_ns,
                             static_cast<unsigned>(i));
            } else {
                std::fprintf(csv, "%" PRIu64 ",%" PRIu64 ",%u,%.6f,%.6f,%.6f,%.6f\n",
                             frame_index,
                             timestamp_ns,
                             static_cast<unsigned>(i),
                             static_cast<double>(pt.x),
                             static_cast<double>(pt.y),
                             static_cast<double>(pt.z),
                             static_cast<double>(pt.intensity));
            }
        }

        result.frames_converted++;
        result.points_converted += num_points;
        frame_index++;

        // Progress reporting.
        if (progress_callback_ && progress_interval_ > 0 &&
            frame_index % progress_interval_ == 0) {
            progress_callback_(frame_index, result.bytes_read, total_bytes);
        }
    }

    std::fclose(csv);
    ::close(fd);

    // Final progress report.
    if (progress_callback_) {
        progress_callback_(frame_index, result.bytes_read, total_bytes);
    }

    result.success = result.error_message.empty();
    return result;
}
