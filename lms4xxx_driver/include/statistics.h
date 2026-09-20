#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>


namespace lms4xxx {
    // Atomic counters updated by the receive and parse threads
    struct DriverStatistics {
        // --- Receive thread counters ---
        std::atomic<std::uint64_t> bytes_received{0}; ///< Total TCP bytes read
        std::atomic<std::uint64_t> frames_received{0}; ///< Complete CoLa B frames extracted
        std::atomic<std::uint64_t> frames_dropped{0}; ///< Frames dropped (ring buffer full)
        std::atomic<std::uint64_t> crc_errors{0}; ///< CRC8 validation failures
        std::atomic<std::uint64_t> framing_errors{0}; ///< Invalid frame structure

        // --- Parse thread counters ---
        std::atomic<std::uint64_t> frames_parsed{0}; ///< Parsed scans that passed configured content checks
        std::atomic<std::uint64_t> non_scan_frames{0}; ///< Decoded CoLa B frames classified as non-scan replies
        std::atomic<std::uint64_t> parse_errors{0}; ///< Parse failures
        std::atomic<std::uint64_t> counter_gaps{0}; ///< Telegram/scan counter discontinuities
        // Frames that decoded cleanly but were neither a scan nor an expected
        // telemetry answer. Non-zero means the link carries something the driver
        // does not model — the only visible symptom of a desynchronised stream.
        std::atomic<std::uint64_t> unexpected_replies{0};
        // Legacy discarded count (now zero: flagged pre-plausibility scans are retained),
        // and largest |device UTC delta - uptime delta| between recorded scans (us).
        std::atomic<std::uint64_t> prelock_scans_discarded{0};
        std::atomic<std::int64_t> max_time_step_us{0};
        std::atomic<std::uint64_t> utc_backwards{0};
        std::atomic<std::uint64_t> utc_repeated{0};
        std::atomic<std::uint64_t> clock_step_events{0};
        std::atomic<std::uint64_t> device_no_ntp_events{0};
        std::atomic<bool> ntp_server_reachable{false};

        // --- Timing (us) ---
        std::atomic<std::uint64_t> last_frame_time_us{0}; ///< Timestamp of last received frame
        std::atomic<std::uint32_t> last_telegram_counter{0};
        std::atomic<std::uint32_t> last_scan_counter{0};

        // CLOCK_REALTIME us when NTP was configured; 0 = disabled / not yet
        std::atomic<std::uint64_t> ntp_configured_at_us{0};

        // Host server reachability is separate. No state below certifies device
        // lock or absolute timestamp accuracy.
        enum class NtpStatus : std::uint8_t {
            kOff = 0, ///< ntp.enabled = false
            kUnverified = 2, ///< plausible device date; absolute time remains unverified
            kNoTimestamp = 3, ///< device streams no timestamp block
            kUnreachable = 4, ///< host cannot reach the server; device's route is not observed
            kNotLocked = 5, ///< no plausible device calendar timestamp seen yet
            kNoSignal = 6, ///< device explicitly reports No NTP signal
            kStale = 7, ///< device warning readback absent, malformed or older than 30 seconds
            kClockAnomaly = 8, ///< observed UTC regression, uptime discontinuity or excessive step
        };

        std::atomic<NtpStatus> ntp_status{NtpStatus::kOff};

        // POD copy for logging
        struct Snapshot {
            std::uint64_t bytes_received;
            std::uint64_t frames_received;
            std::uint64_t frames_dropped;
            std::uint64_t crc_errors;
            std::uint64_t framing_errors;
            std::uint64_t frames_parsed;
            std::uint64_t parse_errors;
            std::uint64_t counter_gaps;
            std::uint64_t unexpected_replies;
            std::uint64_t prelock_scans_discarded;
            std::int64_t max_time_step_us;
            std::uint64_t last_frame_time_us;
            std::uint32_t last_telegram_counter;
            std::uint32_t last_scan_counter;
            std::uint64_t ntp_configured_at_us;
            NtpStatus ntp_status;
            std::uint64_t utc_backwards;
            std::uint64_t utc_repeated;
            std::uint64_t clock_step_events;
            std::uint64_t device_no_ntp_events;
            bool ntp_server_reachable;
            std::uint64_t non_scan_frames;

            // Exclude only frames positively identified as non-scan replies.
            // Undecodable, dropped and pending frames remain candidates, so a
            // receive-ring loss or parse failure cannot improve the rate.
            std::uint64_t ScanCandidateFrames() const {
                return frames_received > non_scan_frames ? frames_received - non_scan_frames : 0;
            }

            // Parsed, content-verified scans / received scan candidates, percent.
            // This is not callback delivery, writer success or end-to-end loss:
            // missing telegrams and frames rejected before extraction have their
            // own counters and never enter frames_received.
            double DeliveryRate() const {
                const auto candidates = ScanCandidateFrames();
                if (candidates == 0)
                    return 0.0;
                return static_cast<double>(frames_parsed) / static_cast<double>(candidates) * 100.0;
            }

        };

        // Relaxed counters from two threads: not a consistent cut (DeliveryRate can transiently exceed 100 %),
        // not a basis for alerting.
        Snapshot GetSnapshot() const {
            return {
                bytes_received.load(std::memory_order_relaxed), frames_received.load(std::memory_order_relaxed),
                frames_dropped.load(std::memory_order_relaxed), crc_errors.load(std::memory_order_relaxed),
                framing_errors.load(std::memory_order_relaxed), frames_parsed.load(std::memory_order_relaxed),
                parse_errors.load(std::memory_order_relaxed), counter_gaps.load(std::memory_order_relaxed),
                unexpected_replies.load(std::memory_order_relaxed),
                prelock_scans_discarded.load(std::memory_order_relaxed),
                max_time_step_us.load(std::memory_order_relaxed),
                last_frame_time_us.load(std::memory_order_relaxed),
                last_telegram_counter.load(std::memory_order_relaxed),
                last_scan_counter.load(std::memory_order_relaxed),
                ntp_configured_at_us.load(std::memory_order_relaxed),
                ntp_status.load(std::memory_order_relaxed),
                utc_backwards.load(std::memory_order_relaxed),
                utc_repeated.load(std::memory_order_relaxed),
                clock_step_events.load(std::memory_order_relaxed),
                device_no_ntp_events.load(std::memory_order_relaxed),
                ntp_server_reachable.load(std::memory_order_relaxed),
                non_scan_frames.load(std::memory_order_relaxed),
            };
        }

        void Reset() {
            bytes_received.store(0, std::memory_order_relaxed);
            frames_received.store(0, std::memory_order_relaxed);
            frames_dropped.store(0, std::memory_order_relaxed);
            crc_errors.store(0, std::memory_order_relaxed);
            framing_errors.store(0, std::memory_order_relaxed);
            frames_parsed.store(0, std::memory_order_relaxed);
            non_scan_frames.store(0, std::memory_order_relaxed);
            parse_errors.store(0, std::memory_order_relaxed);
            counter_gaps.store(0, std::memory_order_relaxed);
            unexpected_replies.store(0, std::memory_order_relaxed);
            prelock_scans_discarded.store(0, std::memory_order_relaxed);
            max_time_step_us.store(0, std::memory_order_relaxed);
            utc_backwards.store(0, std::memory_order_relaxed);
            utc_repeated.store(0, std::memory_order_relaxed);
            clock_step_events.store(0, std::memory_order_relaxed);
            device_no_ntp_events.store(0, std::memory_order_relaxed);
            last_frame_time_us.store(0, std::memory_order_relaxed);
            last_telegram_counter.store(0, std::memory_order_relaxed);
            last_scan_counter.store(0, std::memory_order_relaxed);
            // ntp_* reflect Configure(), which precedes the Reset() in StartScanning()
        }

        DriverStatistics() = default;

        DriverStatistics(const DriverStatistics &) = delete;

        DriverStatistics &operator=(const DriverStatistics &) = delete;

        DriverStatistics(DriverStatistics &&) = delete;

        DriverStatistics &operator=(DriverStatistics &&) = delete;
    };
} // namespace lms4xxx
