#include "sbf_live_recorder.h"

#include <chrono>
#include <string>
#include <cstdio>
#include <limits>

#include "logger.h"
#include "sbf_parsers.h"


namespace asterx {
    namespace {
        constexpr std::string_view kModule = "AsteRx";
        common::DriverLog g_log{std::string(kModule)};

        constexpr auto kFlushInterval = std::chrono::milliseconds(250); // GUI tails at 0.5 s
        constexpr std::size_t kWriteBufferBytes = 64 * 1024;

        std::uint64_t HostUnixNs() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                .count());
        }

        // Scaled integer with a DNU sentinel -> physical double, or NaN
        double Scaled(std::uint16_t v, double factor) {
            return sbf::Valid(v) ? v * factor : std::numeric_limits<double>::quiet_NaN();
        }

        // Bounded snprintf appender; sets ok=false on truncation (row dropped)
        struct Line {
            char buf[768];
            int len{0};
            bool ok{true};

            template<typename... Args>
            void Add(const char *fmt, Args... args) {
                if (!ok) return;
                int n = 0;
                if constexpr (sizeof...(Args) == 0) {
                    // Zero-arg case: never pass a runtime pointer as the format
                    // string (-Wformat-security); copy it verbatim instead.
                    n = std::snprintf(buf + len, sizeof(buf) - static_cast<std::size_t>(len), "%s", fmt);
                } else {
                    n = std::snprintf(buf + len, sizeof(buf) - static_cast<std::size_t>(len), fmt, args...);
                }
                if (n < 0 || len + n >= static_cast<int>(sizeof(buf))) {
                    ok = false;
                    return;
                }
                len += n;
            }

            // Time prefix shared by every CSV: raw GPS time + derived Unix
            // times (gps_unix_ns==0 when tow/wnc are DNU; host always valid)
            void TimePrefix(const sbf::BlockHeader &h, std::uint64_t host_ns) {
                Add("%s,%s,%llu,%llu", sbf::Valid(h.tow) ? std::to_string(h.tow).c_str() : "nan",
                    sbf::Valid(h.wnc) ? std::to_string(h.wnc).c_str() : "nan",
                    static_cast<unsigned long long>(sbf::GpsToUnixNs(h.tow, h.wnc)),
                    static_cast<unsigned long long>(host_ns));
            }
        };
    } // namespace


    bool SbfLiveRecorder::Append(Channel &ch, const char *line, std::size_t len) noexcept {
        if (ch.failed) {
            return false;
        }
        try {
            if (!ch.opened) {
                const std::string path = cfg_.dir + "/" + ch.filename;
                if (!ch.writer.Open(path, kWriteBufferBytes)) {
                    ch.failed = true;
                    g_log.Error("[Live] Cannot open {} — live CSV disabled for this block", path);
                    return false;
                }
                ch.opened = true;
                ch.writer.Write(ch.header, std::char_traits<char>::length(ch.header));
                ch.writer.Write("\n", 1);
            }
            ch.writer.Write(line, len);
            if (!ch.writer.Good()) {
                // IsOpen() stays true on badbit — Good() is the error signal
                ch.failed = true;
                g_log.Error("[Live] Write failed on {} — live CSV disabled for this block", ch.filename);
                return false;
            }
        } catch (...) {
            // BufferedFileWriter does not throw, but the no-throw guarantee of
            // on_block must not rest on that
            ch.failed = true;
            return false;
        }
        ++stats_.rows_written;
        return true;
    }


    void SbfLiveRecorder::NoteParseError() noexcept {
        ++stats_.parse_errors;
        if (stats_.parse_errors == 1 || stats_.parse_errors % 1000 == 0) {
            g_log.Warn("[Live] SBF parse errors: {}", stats_.parse_errors);
        }
    }


    void SbfLiveRecorder::MaybeFlush() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush_ < kFlushInterval) {
            return;
        }
        last_flush_ = now;
        Flush();
    }


    void SbfLiveRecorder::OnBlock(const std::uint8_t *data, std::size_t size) noexcept {
        if (!cfg_.enabled || size < 8) {
            return;
        }
        const std::uint64_t host_ns = HostUnixNs();
        switch (sbf::PeekId(data, size)) {
            case sbf::kIdINSNavGeod:
            case sbf::kIdExtEventINSNavGeod: {
                sbf::INSNavGeod m;
                if (!sbf::ParseINSNavGeod(data, size, m)) {
                    NoteParseError();
                    break;
                }
                Line l;
                l.TimePrefix(m.block_header, host_ns);
                l.Add(",%u,%u,%u,%.2f", static_cast<unsigned>(m.gnss_mode), static_cast<unsigned>(m.error),
                      static_cast<unsigned>(m.info), Scaled(m.gnss_age, 0.01));
                l.Add(",%.10f,%.10f,%.4f,%.4f", m.latitude * sbf::kRadToDeg, m.longitude * sbf::kRadToDeg,
                      m.height, static_cast<double>(m.undulation));
                l.Add(",%.2f,%.6g,%u", Scaled(m.accuracy, 0.01), Scaled(m.latency, 0.0001),
                      static_cast<unsigned>(m.sb_list));
                l.Add(",%.4g,%.4g,%.4g", static_cast<double>(m.latitude_std_dev),
                      static_cast<double>(m.longitude_std_dev), static_cast<double>(m.height_std_dev));
                l.Add(",%.6g,%.6g,%.6g", static_cast<double>(m.heading), static_cast<double>(m.pitch),
                      static_cast<double>(m.roll));
                l.Add(",%.4g,%.4g,%.4g", static_cast<double>(m.heading_std_dev),
                      static_cast<double>(m.pitch_std_dev), static_cast<double>(m.roll_std_dev));
                l.Add(",%.6g,%.6g,%.6g", static_cast<double>(m.ve), static_cast<double>(m.vn),
                      static_cast<double>(m.vu));
                l.Add(",%.4g,%.4g,%.4g\n", static_cast<double>(m.ve_std_dev), static_cast<double>(m.vn_std_dev),
                      static_cast<double>(m.vu_std_dev));
                if (l.ok) {
                    Append(ins_, l.buf, static_cast<std::size_t>(l.len));
                }
                break;
            }
            case sbf::kIdReceiverStatus: {
                sbf::ReceiverStatus m;
                if (!sbf::ParseReceiverStatus(data, size, m)) {
                    NoteParseError();
                    break;
                }
                Line l;
                l.TimePrefix(m.block_header, host_ns);
                // cpu_load DNU = 255 (p.375)
                l.Add(",%s,%u,%u,%u,%u", sbf::Valid(m.cpu_load) ? std::to_string(m.cpu_load).c_str() : "nan",
                      static_cast<unsigned>(m.up_time), static_cast<unsigned>(m.rx_status),
                      static_cast<unsigned>(m.rx_error), static_cast<unsigned>(m.ext_error));
                // Temperature carries an offset of 100 and a Do-Not-Use value of 0 (p.375);
                // subtracting the offset from the sentinel would report -100 degC
                const double temp_c = m.temperature == 0
                                          ? std::numeric_limits<double>::quiet_NaN()
                                          : static_cast<double>(m.temperature) - 100.0;
                l.Add(",%.4g,%u,", temp_c, static_cast<unsigned>(m.cmd_count));
                for (std::size_t i = 0; i < m.agc_state.size(); ++i) {
                    // "frontend:gain;..." — no commas
                    l.Add(i == 0 ? "%u:%d" : ";%u:%d", static_cast<unsigned>(m.agc_state[i].frontend_id),
                          static_cast<int>(m.agc_state[i].gain));
                }
                l.Add("\n");
                if (l.ok) Append(rxs_, l.buf, static_cast<std::size_t>(l.len));
                break;
            }
            default:
                return; // every other Block (measurements, PVT, IMU raw, ...) is .sbf-only
        }
        MaybeFlush();
    }


    void SbfLiveRecorder::Flush() noexcept {
        for (Channel *ch: {&ins_, &rxs_}) {
            if (ch->opened && !ch->failed) {
                ch->writer.Flush();
            }
        }
    }


    void SbfLiveRecorder::close() noexcept {
        for (Channel *ch: {&ins_, &rxs_}) {
            if (ch->opened) {
                ch->writer.Close();
                ch->opened = false;
            }
            ch->failed = true; // never reopen (truncating) after close
        }
    }
} // namespace asterx
