#ifndef LMS4XXX_STATISTICS_H
#define LMS4XXX_STATISTICS_H

#include <atomic>
#include <cstddef>
#include <cstdint>


namespace LMS4xxx {

	// Atomic counters updated in real-time by receive and parse threads.
	struct DriverStatistics {
		// --- Receive thread counters ---
		std::atomic<std::uint64_t> bytes_received{ 0 };		///< Total TCP bytes read
		std::atomic<std::uint64_t> frames_received{ 0 };		///< Complete CoLa B frames extracted
		std::atomic<std::uint64_t> frames_dropped{ 0 };		///< Frames dropped (ring buffer full)
		std::atomic<std::uint64_t> crc_errors{ 0 };			///< CRC8 validation failures
		std::atomic<std::uint64_t> framing_errors{ 0 };		///< Invalid frame structure

		// --- Parse thread counters ---
		std::atomic<std::uint64_t> frames_parsed{ 0 };		///< Successfully parsed scan frames
		std::atomic<std::uint64_t> parse_errors{ 0 };			///< Parse failures
		std::atomic<std::uint64_t> counter_gaps{ 0 };			///< Telegram/scan counter discontinuities

		// --- Timing (microseconds) ---
		std::atomic<std::uint64_t> last_frame_time_us{ 0 };	///< Timestamp of last received frame
		std::atomic<std::uint32_t> last_telegram_counter{ 0 };
		std::atomic<std::uint32_t> last_scan_counter{ 0 };

		// Host wall-clock time (microseconds, CLOCK_REALTIME) when NTP was
		// successfully configured. Zero if NTP is disabled or not yet configured.
		std::atomic<std::uint64_t> ntp_configured_at_us{ 0 };

		// Plain-old-data snapshot for logging and reporting.
		struct Snapshot {
			std::uint64_t bytes_received;
			std::uint64_t frames_received;
			std::uint64_t frames_dropped;
			std::uint64_t crc_errors;
			std::uint64_t framing_errors;
			std::uint64_t frames_parsed;
			std::uint64_t parse_errors;
			std::uint64_t counter_gaps;
			std::uint64_t last_frame_time_us;
			std::uint32_t last_telegram_counter;
			std::uint32_t last_scan_counter;
			std::uint64_t ntp_configured_at_us;

			// Percentage of frames successfully delivered to user callback.
			[[nodiscard]] double DeliveryRate() const {
				if (frames_received == 0)
					return 0.0;
				return static_cast<double>(frames_parsed) / static_cast<double>(frames_received) * 100.0;
			}

			// Total errors (CRC + framing + parse).
			[[nodiscard]] std::uint64_t TotalErrors() const { return crc_errors + framing_errors + parse_errors; }
		};

		// Take a relaxed-consistency snapshot of all counters.
		[[nodiscard]] Snapshot GetSnapshot() const {
			return {
				bytes_received.load(std::memory_order_relaxed),		frames_received.load(std::memory_order_relaxed),
				frames_dropped.load(std::memory_order_relaxed),		crc_errors.load(std::memory_order_relaxed),
				framing_errors.load(std::memory_order_relaxed),		frames_parsed.load(std::memory_order_relaxed),
				parse_errors.load(std::memory_order_relaxed),		counter_gaps.load(std::memory_order_relaxed),
				last_frame_time_us.load(std::memory_order_relaxed), last_telegram_counter.load(std::memory_order_relaxed),
				last_scan_counter.load(std::memory_order_relaxed),
				ntp_configured_at_us.load(std::memory_order_relaxed),
			};
		}

		// Reset all counters to zero.
		void Reset() {
			bytes_received.store(0, std::memory_order_relaxed);
			frames_received.store(0, std::memory_order_relaxed);
			frames_dropped.store(0, std::memory_order_relaxed);
			crc_errors.store(0, std::memory_order_relaxed);
			framing_errors.store(0, std::memory_order_relaxed);
			frames_parsed.store(0, std::memory_order_relaxed);
			parse_errors.store(0, std::memory_order_relaxed);
			counter_gaps.store(0, std::memory_order_relaxed);
			last_frame_time_us.store(0, std::memory_order_relaxed);
			last_telegram_counter.store(0, std::memory_order_relaxed);
			last_scan_counter.store(0, std::memory_order_relaxed);
			ntp_configured_at_us.store(0, std::memory_order_relaxed);
		}

		DriverStatistics() = default;
		DriverStatistics(const DriverStatistics &) = delete;
		DriverStatistics &operator=(const DriverStatistics &) = delete;
		DriverStatistics(DriverStatistics &&) = delete;
		DriverStatistics &operator=(DriverStatistics &&) = delete;
	};

}  // namespace LMS4xxx

#endif	// LMS4XXX_STATISTICS_H
