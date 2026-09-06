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
        std::atomic<std::uint64_t> frames_parsed{0}; ///< Successfully parsed scan frames
        std::atomic<std::uint64_t> parse_errors{0}; ///< Parse failures
        std::atomic<std::uint64_t> counter_gaps{0}; ///< Telegram/scan counter discontinuities
        // Frames that decoded cleanly but were neither a scan nor an expected
        // telemetry answer. Non-zero means the link carries something the driver
        // does not model — the only visible symptom of a desynchronised stream.
        std::atomic<std::uint64_t> unexpected_replies{0};

        // --- Timing (us) ---
        std::atomic<std::uint64_t> last_frame_time_us{0}; ///< Timestamp of last received frame
        std::atomic<std::uint32_t> last_telegram_counter{0};
        std::atomic<std::uint32_t> last_scan_counter{0};

        // CLOCK_REALTIME us when NTP was configured; 0 = disabled / not yet
        std::atomic<std::uint64_t> ntp_configured_at_us{0};

        // Verified by probing the NTP server itself (not the device clock: a
        // previously synced device keeps near-correct time and masks a dead server)
        enum class NtpStatus : std::uint8_t {
            kOff = 0, ///< ntp.enabled = false
            kOk = 2, ///< server answered a healthy SNTP response
            kNoTimestamp = 3, ///< device streams no timestamp block
            kUnreachable = 4, ///< server stopped answering (device clock free-running)
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
            std::uint64_t last_frame_time_us;
            std::uint32_t last_telegram_counter;
            std::uint32_t last_scan_counter;
            std::uint64_t ntp_configured_at_us;
            NtpStatus ntp_status;

            // Frames delivered to the callback, percent
            double DeliveryRate() const {
                if (frames_received == 0)
                    return 0.0;
                return static_cast<double>(frames_parsed) / static_cast<double>(frames_received) * 100.0;
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
                last_frame_time_us.load(std::memory_order_relaxed),
                last_telegram_counter.load(std::memory_order_relaxed),
                last_scan_counter.load(std::memory_order_relaxed),
                ntp_configured_at_us.load(std::memory_order_relaxed),
                ntp_status.load(std::memory_order_relaxed),
            };
        }

        void Reset() {
            bytes_received.store(0, std::memory_order_relaxed);
            frames_received.store(0, std::memory_order_relaxed);
            frames_dropped.store(0, std::memory_order_relaxed);
            crc_errors.store(0, std::memory_order_relaxed);
            framing_errors.store(0, std::memory_order_relaxed);
            frames_parsed.store(0, std::memory_order_relaxed);
            parse_errors.store(0, std::memory_order_relaxed);
            counter_gaps.store(0, std::memory_order_relaxed);
            unexpected_replies.store(0, std::memory_order_relaxed);
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

