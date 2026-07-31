#ifndef ASTERX_SBF_LIVE_RECORDER_HPP
#define ASTERX_SBF_LIVE_RECORDER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "file_writer.h"


namespace asterx {
    class SbfLiveRecorder {
    public:
        struct Config {
            std::string dir; // <session>/bin/asterx
            bool enabled{true}; // output.live_csv
        };

        struct Stats {
            std::uint64_t rows_written{0};
            std::uint64_t parse_errors{0};
            std::uint64_t writers_failed{0};
        };

        explicit SbfLiveRecorder(Config cfg) : cfg_(std::move(cfg)) {
        } // no I/O; files open lazily

        // One complete SBF frame (CRC already validated by ssnrx)
        void on_block(const std::uint8_t *data, std::size_t size) noexcept;

        // Link-down segment boundary: make buffered rows visible to the GUI
        // tailer; files stay open and continue across the reconnect
        void flush() noexcept;

        // Shutdown: flush + close everything (idempotent)
        void close() noexcept;

        [[nodiscard]] const Stats &stats() const noexcept { return stats_; }

    private:
        struct Channel {
            const char *filename; // e.g. "live_pvtgeodetic.csv"
            const char *header; // header row, without trailing newline
            Common::BufferedFileWriter writer;
            bool opened{false};
            bool failed{false};
        };

        bool append_(Channel &ch, const char *line, std::size_t len) noexcept;

        void note_parse_error_() noexcept;

        void maybe_flush_() noexcept;

        Config cfg_;
        Stats stats_;
        std::chrono::steady_clock::time_point last_flush_{};
        Channel pvt_{
            "live_pvtgeodetic.csv",
            "tow_ms,wnc,gps_unix_ns,host_unix_ns,mode,error,nr_sv,lat_deg,lon_deg,height_m,undulation_m,"
            "vn_mps,ve_mps,vu_mps,cog_deg,h_acc_m,v_acc_m,mean_corr_age_s,alert_flag"
        };
        Channel ext_{
            "live_extsensormeas.csv",
            "tow_ms,wnc,gps_unix_ns,host_unix_ns,acc_x_mps2,acc_y_mps2,acc_z_mps2,gyro_x_degps,"
            "gyro_y_degps,gyro_z_degps,temp_c,zero_vel_flag"
        };
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
        Channel att_{
            "live_atteuler.csv",
            "tow_ms,wnc,gps_unix_ns,host_unix_ns,nr_sv,error,mode,heading_deg,pitch_deg,roll_deg,"
            "heading_dot_degps,pitch_dot_degps,roll_dot_degps"
        };
    };
} // namespace asterx

#endif	// ASTERX_SBF_LIVE_RECORDER_HPP
