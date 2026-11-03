#include "scan_time.h"


namespace lms4xxx {
    namespace {
        // Hinnant's days_from_civil; no host wall clock or timezone is consulted.
        std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
            y -= m <= 2;
            const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
            const auto yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
        }
    }

    std::int64_t DeviceTimeUnixUs(const ScanTimestamp &ts) {
        if (ts.month < 1 || ts.month > 12 || ts.day < 1 || ts.day > 31 || ts.hour > 23 || ts.minute > 59 ||
            ts.second > 60 || ts.microsecond > 999999) {
            return 0;
        }
        const std::int64_t days = DaysFromCivil(ts.year, ts.month, ts.day);
        const std::int64_t secs = days * 86400 + ts.hour * 3600 + ts.minute * 60 + ts.second;
        return secs * 1000000 + static_cast<std::int64_t>(ts.microsecond);
    }
}
