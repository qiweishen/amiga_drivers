#pragma once

#include <cstdint>

namespace ebus_driver {
    // SDK-free GVSP continuity accounting. Select the mode from verified device
    // configuration, never from an observed ID alone. In 16-bit mode the valid wire
    // cycle is 1..65535; forward distances above half a cycle are ambiguous and are
    // anomalies rather than invented large loss counts. Every observation is still
    // recorded by the caller, including duplicates and backwards IDs.
    class BlockIds {
    public:
        struct Observation {
            std::uint64_t gap = 0;
            bool anomaly = false;
        };

        explicit BlockIds(bool extended = false) : extended_(extended) {
        }

        Observation Observe(std::uint64_t id) {
            Observation result;
            if (id == 0 || (!extended_ && id > 65535)) {
                result.anomaly = true;
                return result;
            }
            if (!have_last_) {
                have_last_ = true;
                last_ = id;
                return result;
            }
            if (extended_) {
                if (id <= last_) {
                    result.anomaly = true;
                    return result;
                }
                result.gap = id - last_ - 1;
            } else {
                const std::uint64_t expected = last_ == 65535 ? 1 : last_ + 1;
                const std::uint64_t distance = (id + 65535 - expected) % 65535;
                if (distance > 32767) {
                    result.anomaly = true;
                    return result;
                }
                result.gap = distance;
            }
            last_ = id;
            return result;
        }

    private:
        bool extended_ = false, have_last_ = false;
        std::uint64_t last_ = 0;
    };
} // namespace ebus_driver
