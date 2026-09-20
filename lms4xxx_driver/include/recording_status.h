#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scan_record_writer.h"
#include "statistics.h"

namespace lms4xxx {
    // Online fail-fast checks data loss only. Time faults have their own grace
    // period and stop policy; final time quality is assessed after draining.
    inline const char *FirstDataLoss(const DriverStatistics::Snapshot &drv,
                                     const ScanRecordWriter::Statistics &wr) {
        if (drv.frames_dropped != 0) return "receive ring overflow";
        if (wr.frames_dropped != 0) return "writer rejected or dropped scans";
        if (drv.counter_gaps != 0) return "telegram counter gap";
        if (drv.crc_errors != 0) return "frame checksum error";
        if (drv.framing_errors != 0) return "framing error";
        if (drv.parse_errors != 0) return "parse error";
        return nullptr;
    }

    struct RecordingStatus {
        // False means no error was identified here, not proof of end-to-end
        // completeness or absolute time accuracy. Driver faults remain separate.
        bool data_integrity_failed = false;
        bool time_quality_degraded = false;
        std::vector<std::string> failure_reasons;

        bool Failed() const { return !failure_reasons.empty(); }

        std::string Summary() const {
            std::string summary;
            for (const auto &reason : failure_reasons) {
                if (!summary.empty()) summary += "; ";
                summary += reason;
            }
            return summary;
        }
    };

    // Final snapshots only: receive/parse/writer threads must have joined.
    // Keep the strict time-quality verdict, but report it independently of
    // payload loss and teardown so an orderly operator stop is not ambiguous.
    inline RecordingStatus AssessRecording(const DriverStatistics::Snapshot &drv,
                                             const ScanRecordWriter::Statistics &wr,
                                             bool driver_fault, bool writer_failed, bool already_failed) {
        RecordingStatus result;
        // Also expose a bad final clock state before its grace period expires.
        // This flag is diagnostic; only the established fault/counter policy
        // below rejects the run, and absence of degradation never verifies UTC.
        using NtpStatus = DriverStatistics::NtpStatus;
        result.time_quality_degraded = drv.ntp_status == NtpStatus::kNoTimestamp ||
            drv.ntp_status == NtpStatus::kNotLocked || drv.ntp_status == NtpStatus::kNoSignal ||
            drv.ntp_status == NtpStatus::kUnreachable || drv.ntp_status == NtpStatus::kClockAnomaly;
        const auto data_error = [&](const char *name, std::uint64_t count) {
            if (count == 0) return;
            result.data_integrity_failed = true;
            result.failure_reasons.emplace_back(std::string(name) + "=" + std::to_string(count));
        };
        data_error("dropped_ring", drv.frames_dropped);
        data_error("dropped_writer", wr.frames_dropped);
        data_error("counter_gaps", drv.counter_gaps);
        data_error("crc_errors", drv.crc_errors);
        data_error("framing_errors", drv.framing_errors);
        data_error("parse_errors", drv.parse_errors);
        if (writer_failed) {
            result.data_integrity_failed = true;
            result.failure_reasons.emplace_back("writer failure (see Writer error)");
        }
        if (drv.frames_parsed != wr.frames_queued || wr.frames_queued != wr.frames_written) {
            result.data_integrity_failed = true;
            result.failure_reasons.emplace_back("scan recording count mismatch: parsed=" +
                std::to_string(drv.frames_parsed) + " queued=" + std::to_string(wr.frames_queued) +
                " written=" + std::to_string(wr.frames_written));
        }

        const auto time_error = [&](const char *name, std::uint64_t count) {
            if (count == 0) return;
            result.time_quality_degraded = true;
            result.failure_reasons.emplace_back(std::string(name) + "=" + std::to_string(count));
        };
        time_error("device reported No NTP signal: ntp_device_loss", drv.device_no_ntp_events);
        time_error("UTC regression: utc_back", drv.utc_backwards);
        time_error("device clock discontinuity: clock_step_events", drv.clock_step_events);
        if (driver_fault) result.failure_reasons.emplace_back("driver fault (see preceding driver error)");
        if (already_failed && !result.Failed())
            result.failure_reasons.emplace_back("earlier run/shutdown failure (see preceding error)");
        return result;
    }
} // namespace lms4xxx
