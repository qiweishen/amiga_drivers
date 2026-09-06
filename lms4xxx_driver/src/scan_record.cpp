#include "scan_record.h"

#include <algorithm>
#include <cstring>


namespace lms4xxx {
    namespace {
        // Hinnant's days_from_civil
        std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
            y -= m <= 2;
            const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
            const auto yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
        }

        const ChannelData16 *Channel16(const ScanData &scan, std::uint16_t bit) {
            switch (bit) {
                case ChannelMask::kDist1:
                    return scan.DistanceChannel();
                case ChannelMask::kRssi1:
                    return scan.RssiChannel();
                case ChannelMask::kRefl1:
                    return scan.ReflectanceChannel();
                case ChannelMask::kAngl1:
                    return scan.AngleCorrectionChannel();
                default:
                    return nullptr;
            }
        }

        std::uint16_t *Array16(ScanRecord &record, std::uint16_t bit) {
            switch (bit) {
                case ChannelMask::kDist1:
                    return record.dist;
                case ChannelMask::kRssi1:
                    return record.rssi;
                case ChannelMask::kRefl1:
                    return record.refl;
                case ChannelMask::kAngl1:
                    return record.angl;
                default:
                    return nullptr;
            }
        }

        // Copy the points, zero the rest of the row
        template<typename T>
        void CopyPoints(const std::vector<T> *src, std::uint16_t num_data, T *dst) {
            std::size_t n = 0;
            if (src) {
                n = std::min<std::size_t>({num_data, src->size(), kMaxPointsPerScan});
                if (n > 0) {
                    std::memcpy(dst, src->data(), n * sizeof(T));
                }
            }
            if (n < kMaxPointsPerScan) {
                std::memset(dst + n, 0, (kMaxPointsPerScan - n) * sizeof(T));
            }
        }
    } // namespace


    std::int64_t DeviceTimeUnixUs(const ScanTimestamp &ts) {
        if (ts.month < 1 || ts.month > 12 || ts.day < 1 || ts.day > 31 || ts.hour > 23 || ts.minute > 59 ||
            ts.second > 60 || ts.microsecond > 999999) {
            return 0;
        }
        const std::int64_t days = DaysFromCivil(ts.year, ts.month, ts.day);
        const std::int64_t secs = days * 86400 + ts.hour * 3600 + ts.minute * 60 + ts.second;
        return secs * 1000000 + static_cast<std::int64_t>(ts.microsecond);
    }


    void FillScanRecord(const ScanData &scan, std::uint16_t mask, ScanRecord &out) {
        FrameMeta &m = out.meta;
        m = FrameMeta{};

        m.time_since_startup_us = scan.time_since_startup_us;
        m.transmission_time_us = scan.transmission_time_us;
        m.telegram_counter = scan.telegram_counter;
        m.scan_counter = scan.scan_counter;

        // Geometry from the distance channel, else the first channel present
        if (const auto *dist = scan.DistanceChannel()) {
            m.num_points = dist->num_data;
            m.start_angle = dist->start_angle;
            m.angle_step = dist->angle_step;
        } else if (!scan.channels_16bit.empty()) {
            const auto &ch = scan.channels_16bit.front();
            m.num_points = ch.num_data;
            m.start_angle = ch.start_angle;
            m.angle_step = ch.angle_step;
        } else if (!scan.channels_8bit.empty()) {
            const auto &ch = scan.channels_8bit.front();
            m.num_points = ch.num_data;
            m.start_angle = ch.start_angle;
            m.angle_step = ch.angle_step;
        }
        m.num_points = static_cast<std::uint16_t>(std::min<std::size_t>(m.num_points, kMaxPointsPerScan));

        m.scan_frequency = scan.scan_frequency;
        m.measurement_frequency = scan.measurement_frequency;

        m.device_version = scan.device_info.version_number;
        m.device_number = scan.device_info.device_number;
        m.serial_number = scan.device_info.serial_number;
        m.device_status_1 = static_cast<std::uint8_t>(scan.device_info.device_status_1);
        m.device_status_2 = static_cast<std::uint8_t>(scan.device_info.device_status_2);

        m.digital_input_1 = scan.digital_input_1;
        m.digital_input_2 = scan.digital_input_2;
        m.digital_output_1 = scan.digital_output_1;
        m.digital_output_2 = scan.digital_output_2;

        m.has_encoder = scan.has_encoder ? 1 : 0;
        if (scan.has_encoder) {
            m.encoder_position = scan.encoder.position;
        }

        m.has_timestamp = scan.has_timestamp ? 1 : 0;
        if (scan.has_timestamp) {
            m.ts_year = scan.timestamp.year;
            m.ts_month = scan.timestamp.month;
            m.ts_day = scan.timestamp.day;
            m.ts_hour = scan.timestamp.hour;
            m.ts_minute = scan.timestamp.minute;
            m.ts_second = scan.timestamp.second;
            m.ts_microsecond = scan.timestamp.microsecond;
            m.device_time_unix_us = DeviceTimeUnixUs(scan.timestamp);
        }

        m.y_rotation = scan.y_rotation;
        m.has_device_name = scan.has_device_name ? 1 : 0;
        if (scan.has_device_name) {
            const auto copy_len = std::min(scan.device_name.size(), sizeof(m.device_name) - 1);
            std::memcpy(m.device_name, scan.device_name.data(), copy_len);
        }

        for (const auto &spec: kChannelSpecs) {
            if (!(mask & spec.bit)) {
                continue;
            }
            if (spec.bit == ChannelMask::kQlty1) {
                const auto *ch = scan.QualityChannel();
                CopyPoints<std::uint8_t>(ch ? &ch->data : nullptr, ch ? ch->num_data : 0, out.qlty);
            } else {
                const auto *ch = Channel16(scan, spec.bit);
                CopyPoints<std::uint16_t>(ch ? &ch->data : nullptr, ch ? ch->num_data : 0, Array16(out, spec.bit));
            }
        }
    }


    // Packed on-disk width of one /frames row: the sum of the FILE types, not
    // sizeof(FrameMeta). The struct carries 14 bytes of alignment padding that
    // never reach the file, so using sizeof() made both the bytes= statistic and
    // the max_file_bytes split threshold overstate the payload.
    std::uint32_t FrameMetaBytesOnDisk() {
        return static_cast<std::uint32_t>(
            sizeof(FrameMeta::device_time_unix_us) + sizeof(FrameMeta::time_since_startup_us) +
            sizeof(FrameMeta::transmission_time_us) + sizeof(FrameMeta::telegram_counter) +
            sizeof(FrameMeta::scan_counter) + sizeof(FrameMeta::num_points) + sizeof(FrameMeta::start_angle) +
            sizeof(FrameMeta::angle_step) + sizeof(FrameMeta::scan_frequency) +
            sizeof(FrameMeta::measurement_frequency) + sizeof(FrameMeta::device_version) +
            sizeof(FrameMeta::device_number) + sizeof(FrameMeta::serial_number) +
            sizeof(FrameMeta::device_status_1) + sizeof(FrameMeta::device_status_2) +
            sizeof(FrameMeta::digital_input_1) + sizeof(FrameMeta::digital_input_2) +
            sizeof(FrameMeta::digital_output_1) + sizeof(FrameMeta::digital_output_2) +
            sizeof(FrameMeta::has_encoder) + sizeof(FrameMeta::encoder_position) +
            sizeof(FrameMeta::has_timestamp) + sizeof(FrameMeta::ts_year) +
            sizeof(FrameMeta::ts_month) + sizeof(FrameMeta::ts_day) + sizeof(FrameMeta::ts_hour) +
            sizeof(FrameMeta::ts_minute) + sizeof(FrameMeta::ts_second) + sizeof(FrameMeta::ts_microsecond) +
            sizeof(FrameMeta::y_rotation) + sizeof(FrameMeta::has_device_name) + sizeof(FrameMeta::device_name));
    }


    std::uint32_t RecordBytes(std::uint16_t mask) {
        std::uint32_t bytes = FrameMetaBytesOnDisk();
        for (const auto &spec: kChannelSpecs) {
            if (mask & spec.bit) {
                bytes += static_cast<std::uint32_t>(kMaxPointsPerScan * spec.elem_size);
            }
        }
        return bytes;
    }


    std::string ChannelNames(std::uint16_t mask) {
        std::string names;
        for (const auto &spec: kChannelSpecs) {
            if (mask & spec.bit) {
                if (!names.empty()) {
                    names += ',';
                }
                names += spec.name;
            }
        }
        return names;
    }
} // namespace lms4xxx
