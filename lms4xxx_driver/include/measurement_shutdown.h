#pragma once

#include <string_view>
#include <system_error>

namespace lms4xxx {
    struct MeasurementShutdownStatus {
        // A start command may take effect even if its acknowledgement is lost.
        bool measurement_start_requested = false;
        bool cleanup_attempted = false;
        bool standby_command_attempted = false;
        bool standby_confirmed = false;
    };

    // Owner-thread policy shared by command-phase rollback and streaming shutdown.
    // The caller supplies the existing command transport; no device I/O lives here.
    class MeasurementShutdown {
    public:
        void Arm() {
            status_ = {};
            status_.measurement_start_requested = true;
            result_.clear();
        }

        [[nodiscard]] bool Pending() const {
            return status_.measurement_start_requested && !status_.cleanup_attempted;
        }

        template<typename CallMethod>
        std::error_code Park(CallMethod &&call) {
            if (!Pending()) return result_;
            status_.cleanup_attempted = true;
            try {
                // Run logs out. Login succeeds on 1; LMCstandby succeeds on 0.
                result_ = call(std::string_view{"SetAccessMode"}, 0x01);
                if (!result_) {
                    status_.standby_command_attempted = true;
                    result_ = call(std::string_view{"LMCstandby"}, 0x00);
                    status_.standby_confirmed = !result_;
                }
            } catch (...) {
                // Cleanup must remain unsuccessful if transport/setup throws.
                result_ = std::make_error_code(std::errc::io_error);
            }
            return result_;
        }

        [[nodiscard]] MeasurementShutdownStatus Status() const { return status_; }

    private:
        MeasurementShutdownStatus status_;
        std::error_code result_;
    };
} // namespace lms4xxx
