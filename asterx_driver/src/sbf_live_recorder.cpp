#include "sbf_live_recorder.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "logger.h"
#include "sbf_parsers.hpp"


namespace asterx {
	namespace {
		constexpr std::string_view kModule = "AsteRx";
		Common::DriverLog g_log{ std::string(kModule) };

		constexpr auto kFlushInterval = std::chrono::milliseconds(250);	 // GUI tails at 0.5 s
		constexpr std::size_t kWriteBufferBytes = 64 * 1024;

		std::uint64_t host_unix_ns() {
			return static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::system_clock::now().time_since_epoch())
							.count());
		}

		// Scaled integer with a DNU sentinel -> physical double, or NaN
		double scaled(std::uint16_t v, double factor) {
			return sbf::Valid(v) ? v * factor : std::numeric_limits<double>::quiet_NaN();
		}

		// Bounded snprintf appender; sets ok=false on truncation (row dropped)
		struct Line {
			char buf[768];
			int len{ 0 };
			bool ok{ true };

			template<typename... Args>
			void add(const char *fmt, Args... args) {
				if (!ok) return;
				const int n = std::snprintf(buf + len, sizeof(buf) - static_cast<std::size_t>(len), fmt, args...);
				if (n < 0 || len + n >= static_cast<int>(sizeof(buf))) {
					ok = false;
					return;
				}
				len += n;
			}
			// Time prefix shared by every CSV: raw GPS time + derived Unix
			// times (gps_unix_ns==0 when tow/wnc are DNU; host always valid)
			void time_prefix(const sbf::BlockHeader &h, std::uint64_t host_ns) {
				add("%u,%u,%llu,%llu", h.tow, h.wnc,
					static_cast<unsigned long long>(sbf::GpsToUnixNs(h.tow, h.wnc)),
					static_cast<unsigned long long>(host_ns));
			}
		};
	}  // namespace


	bool SbfLiveRecorder::append_(Channel &ch, const char *line, std::size_t len) noexcept {
		if (ch.failed) {
			return false;
		}
		try {
			if (!ch.opened) {
				const std::string path = cfg_.dir + "/" + ch.filename;
				if (!ch.writer.Open(path, kWriteBufferBytes)) {
					ch.failed = true;
					++stats_.writers_failed;
					g_log.error("[Live] Cannot open {} — live CSV disabled for this block", path);
					return false;
				}
				ch.opened = true;
				ch.writer.Write(ch.header, std::char_traits<char>::length(ch.header));
				ch.writer.Write("\n", 1);
			}
			ch.writer.Write(line, len);
			if (!ch.writer.Good()) {  // IsOpen() stays true on badbit — Good() is the error signal
				ch.failed = true;
				++stats_.writers_failed;
				g_log.error("[Live] Write failed on {} — live CSV disabled for this block", ch.filename);
				return false;
			}
		} catch (...) {
			// BufferedFileWriter does not throw, but the no-throw guarantee of
			// on_block must not rest on that
			ch.failed = true;
			++stats_.writers_failed;
			return false;
		}
		++stats_.rows_written;
		return true;
	}


	void SbfLiveRecorder::note_parse_error_() noexcept {
		++stats_.parse_errors;
		if (stats_.parse_errors == 1 || stats_.parse_errors % 1000 == 0) {
			g_log.warn("[Live] SBF parse errors: {}", stats_.parse_errors);
		}
	}


	void SbfLiveRecorder::maybe_flush_() noexcept {
		const auto now = std::chrono::steady_clock::now();
		if (now - last_flush_ < kFlushInterval) {
			return;
		}
		last_flush_ = now;
		flush();
	}


	void SbfLiveRecorder::on_block(const std::uint8_t *data, std::size_t size) noexcept {
		if (!cfg_.enabled || size < 8) {
			return;
		}
		const std::uint64_t host_ns = host_unix_ns();
		switch (sbf::PeekId(data, size)) {
			case sbf::kIdPVTGeodetic: {
				sbf::PVTGeodetic m;
				if (!sbf::ParsePVTGeodetic(data, size, m)) {
					note_parse_error_();
					break;
				}
				Line l;
				l.time_prefix(m.block_header, host_ns);
				l.add(",%u,%u,%u", m.mode, m.error, m.nr_sv);
				l.add(",%.10f,%.10f,%.4f,%.4f", m.latitude * sbf::kRadToDeg, m.longitude * sbf::kRadToDeg,
					  m.height, static_cast<double>(m.undulation));
				l.add(",%.4g,%.4g,%.4g,%.4g", static_cast<double>(m.vn), static_cast<double>(m.ve),
					  static_cast<double>(m.vu), static_cast<double>(m.cog));
				l.add(",%.4g,%.4g,%.4g,%u\n", scaled(m.h_accuracy, 0.01), scaled(m.v_accuracy, 0.01),
					  scaled(m.mean_corr_age, 0.01), m.alert_flag);
				if (l.ok) append_(pvt_, l.buf, static_cast<std::size_t>(l.len));
				break;
			}
			case sbf::kIdExtSensorMeas: {
				sbf::ExtSensorMeas m;
				if (!sbf::ParseExtSensorMeas(data, size, m)) {
					note_parse_error_();
					break;
				}
				Line l;
				l.time_prefix(m.block_header, host_ns);
				l.add(",%.8g,%.8g,%.8g", m.acceleration_x, m.acceleration_y, m.acceleration_z);
				l.add(",%.8g,%.8g,%.8g", m.angular_rate_x, m.angular_rate_y, m.angular_rate_z);
				l.add(",%.4g,%.4g\n", static_cast<double>(m.sensor_temperature), m.zero_velocity_flag);
				if (l.ok) append_(ext_, l.buf, static_cast<std::size_t>(l.len));
				break;
			}
			case sbf::kIdINSNavGeod:
			case sbf::kIdExtEventINSNavGeod: {
				sbf::INSNavGeod m;
				if (!sbf::ParseINSNavGeod(data, size, m)) {
					note_parse_error_();
					break;
				}
				Line l;
				l.time_prefix(m.block_header, host_ns);
				l.add(",%u,%u,%u,%.4g", m.gnss_mode, m.error, m.info, scaled(m.gnss_age, 0.01));
				l.add(",%.10f,%.10f,%.4f,%.4f", m.latitude * sbf::kRadToDeg, m.longitude * sbf::kRadToDeg,
					  m.height, static_cast<double>(m.undulation));
				l.add(",%.4g,%.6g,%u", scaled(m.accuracy, 0.01), scaled(m.latency, 0.0001), m.sb_list);
				l.add(",%.4g,%.4g,%.4g", static_cast<double>(m.latitude_std_dev),
					  static_cast<double>(m.longitude_std_dev), static_cast<double>(m.height_std_dev));
				l.add(",%.6g,%.6g,%.6g", static_cast<double>(m.heading), static_cast<double>(m.pitch),
					  static_cast<double>(m.roll));
				l.add(",%.4g,%.4g,%.4g", static_cast<double>(m.heading_std_dev),
					  static_cast<double>(m.pitch_std_dev), static_cast<double>(m.roll_std_dev));
				l.add(",%.6g,%.6g,%.6g", static_cast<double>(m.ve), static_cast<double>(m.vn),
					  static_cast<double>(m.vu));
				l.add(",%.4g,%.4g,%.4g\n", static_cast<double>(m.ve_std_dev), static_cast<double>(m.vn_std_dev),
					  static_cast<double>(m.vu_std_dev));
				if (l.ok) append_(ins_, l.buf, static_cast<std::size_t>(l.len));
				break;
			}
			case sbf::kIdReceiverStatus: {
				sbf::ReceiverStatus m;
				if (!sbf::ParseReceiverStatus(data, size, m)) {
					note_parse_error_();
					break;
				}
				Line l;
				l.time_prefix(m.block_header, host_ns);
				l.add(",%u,%u,%u,%u,%u", m.cpu_load, m.up_time, m.rx_status, m.rx_error, m.ext_error);
				l.add(",%d,%u,", static_cast<int>(m.temperature) - 100, m.cmd_count);
				for (std::size_t i = 0; i < m.agc_state.size(); ++i) {	// "frontend:gain;..." — no commas
					l.add(i == 0 ? "%u:%d" : ";%u:%d", m.agc_state[i].frontend_id,
						  static_cast<int>(m.agc_state[i].gain));
				}
				l.add("\n");
				if (l.ok) append_(rxs_, l.buf, static_cast<std::size_t>(l.len));
				break;
			}
			case sbf::kIdAttEuler: {
				sbf::AttEuler m;
				if (!sbf::ParseAttEuler(data, size, m)) {
					note_parse_error_();
					break;
				}
				Line l;
				l.time_prefix(m.block_header, host_ns);
				l.add(",%u,%u,%u", m.nr_sv, m.error, m.mode);
				l.add(",%.6g,%.6g,%.6g", static_cast<double>(m.heading), static_cast<double>(m.pitch),
					  static_cast<double>(m.roll));
				l.add(",%.6g,%.6g,%.6g\n", static_cast<double>(m.heading_dot), static_cast<double>(m.pitch_dot),
					  static_cast<double>(m.roll_dot));
				if (l.ok) append_(att_, l.buf, static_cast<std::size_t>(l.len));
				break;
			}
			default:
				return;	 // other blocks (MeasEpoch, ephemerides, ...) are .sbf-only
		}
		maybe_flush_();
	}


	void SbfLiveRecorder::flush() noexcept {
		for (Channel *ch : { &pvt_, &ext_, &ins_, &rxs_, &att_ }) {
			if (ch->opened && !ch->failed) {
				ch->writer.Flush();
			}
		}
	}


	void SbfLiveRecorder::close() noexcept {
		for (Channel *ch : { &pvt_, &ext_, &ins_, &rxs_, &att_ }) {
			if (ch->opened) {
				ch->writer.Close();
				ch->opened = false;
			}
		}
	}
}  // namespace asterx
