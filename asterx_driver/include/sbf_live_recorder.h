#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "file_writer.h"


namespace asterx {
    class SbfLiveRecorder {
    public:
        struct Config {
            std::string dir; // <session>/raw/asterx
            bool enabled{true}; // output.live_csv
        };

        struct Stats {
            std::uint64_t rows_written{0};
            std::uint64_t parse_errors{0};
        };

        explicit SbfLiveRecorder(Config cfg) : cfg_(std::move(cfg)) {
        } // no I/O; files open lazily

        // One complete SBF frame (CRC already validated by ssnrx)
        void OnBlock(const std::uint8_t *data, std::size_t size) noexcept;

        // Link-down segment boundary: make buffered rows visible to the GUI
        // tailer; files stay open and continue across the reconnect
        void Flush() noexcept;

        // Shutdown: flush + close everything (idempotent)
        void close() noexcept;

        const Stats &GetStats() const noexcept { return stats_; }

    private:
        struct Channel {
            const char *filename; // e.g. "live_insnavgeod.csv"
            const char *header; // header row, without trailing newline
            common::BufferedFileWriter writer;
            bool opened{false};
            bool failed{false};
        };

        bool Append(Channel &ch, const char *line, std::size_t len) noexcept;

        void NoteParseError() noexcept;

        void MaybeFlush() noexcept;

        Config cfg_;
        Stats stats_;
        std::chrono::steady_clock::time_point last_flush_{};
        // Only the two blocks the GUI needs: INSNavGeod (position + attitude)
        // and ReceiverStatus (health); everything else is .sbf-only
        Channel ins_{
            "live_insnavgeod.csv",
            "tow_ms,wnc,gps_unix_ns,host_unix_ns,gnss_mode,error,info,gnss_age_s,lat_deg,lon_deg,"
            "height_m,undulation_m,accuracy_m,latency_s,sb_list,lat_std_m,lon_std_m,height_std_m,"
            "heading_deg,pitch_deg,roll_deg,heading_std_deg,pitch_std_deg,roll_std_deg,ve_mps,vn_mps,"
            "vu_mps,ve_std_mps,vn_std_mps,vu_std_mps"
        };
        Channel rxs_{
            "live_receiverstatus.csv",
            "tow_ms,wnc,gps_unix_ns,host_unix_ns,cpu_load_pct,up_time_s,rx_status,rx_error,ext_error,"
            "temp_c,cmd_count,agc"
        };
    };
} // namespace asterx
