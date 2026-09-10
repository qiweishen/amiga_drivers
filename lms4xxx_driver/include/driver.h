#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

#include "callbacks.h"
#include "app_config.h"
#include "device_report.h"
#include "statistics.h"


namespace lms4xxx {
    // Read-only identity (sRN DeviceIdent / DIornr / DItype / LocationName), valid after Configure()
    struct DeviceIdentity {
        std::string firmware_designation;
        std::string firmware_version;
        std::string order_number;
        std::string device_type;
        std::string location_name; ///< device name in the telegram; default = serial number
        bool is_s01_variant = false; ///< LMS4124R-13000S01 (order 1116198): quality-flagged points stay valid
    };


    // SICK LMS4xxx driver: connect -> configure -> stream, callbacks from the parse thread
    class Lms4xxxDriver {
    public:
        explicit Lms4xxxDriver(const DriverConfig &config);

        ~Lms4xxxDriver();

        Lms4xxxDriver(const Lms4xxxDriver &) = delete;

        Lms4xxxDriver &operator=(const Lms4xxxDriver &) = delete;

        Lms4xxxDriver(Lms4xxxDriver &&) = delete;

        Lms4xxxDriver &operator=(Lms4xxxDriver &&) = delete;

        std::error_code Connect();

        std::error_code Configure();

        // sEN LMDscandata 1
        std::error_code StartScanning();

        // sEN LMDscandata 0
        std::error_code StopScanning();

        void Disconnect();

        // Invoked on the parse thread
        void SetScanCallback(ScanDataCallback callback);

        void SetErrorCallback(ErrorCallback callback);

        [[nodiscard]] bool IsScanning() const;

        // Microseconds since the last scan telegram arrived; 0 before the first
        // one. The owner uses it as the stall watchdog: a device that keeps the
        // TCP connection open but stops streaming is otherwise indistinguishable
        // from an idle one, and the run would never end.
        [[nodiscard]] std::uint64_t MicrosSinceLastFrame() const;

        // Fatal fault (first-scan verification, NTP, receive channel); the owner
        // polls it and stops the run — teardown stays on the owner's thread
        [[nodiscard]] bool HasFault() const;

        [[nodiscard]] DriverStatistics::Snapshot GetStatistics() const;

        [[nodiscard]] const DeviceIdentity &GetDeviceIdentity() const;

        // Filled at the end of Configure(); empty fields = the device did not answer
        [[nodiscard]] const DeviceAudit &GetDeviceAudit() const;

        // Ask the device for one telemetry round WHILE STREAMING. Control and
        // data share one TCP connection and the receive thread owns the socket,
        // so this only WRITES the queries (they need no login, manual p.69); the
        // answers come back as ordinary frames and are decoded on the parse
        // thread. Call from the owner thread only.
        void RequestTelemetry();

        // Hands over the most recent answers, if any arrived since the last call.
        bool TakeTelemetry(TelemetrySample &out);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lms4xxx

