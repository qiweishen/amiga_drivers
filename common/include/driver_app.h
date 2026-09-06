#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <nlohmann/json.hpp>


namespace common {
    class IDriverApp {
    public:
        IDriverApp() = default;

        virtual ~IDriverApp() = default;

        // Non-copyable, non-movable (owns threads and I/O resources)
        IDriverApp(const IDriverApp &) = delete;

        IDriverApp &operator=(const IDriverApp &) = delete;

        IDriverApp(IDriverApp &&) = delete;

        IDriverApp &operator=(IDriverApp &&) = delete;

        virtual bool Init(const std::function<bool()> &external_stop = {}) = 0;

        virtual void Run() = 0;

        virtual void Shutdown() = 0;

        // Main reads this only after Run threads have joined and Shutdown has
        // finished. Drivers may retain their native loss-accounting fields here.
        virtual nlohmann::ordered_json FinalStatistics() const { return nlohmann::ordered_json::object(); }

        // Sticky across Run/Shutdown: an operator stop must not hide an I/O
        // failure discovered while draining queues or closing files.
        bool HasFailed() const { return failed_.load(std::memory_order_acquire); }

        void RequestFailure() {
            MarkFailed();
            terminate_.store(true, std::memory_order_release);
        }

        // Microseconds since this driver last received sensor data (steady clock),
        // for the unified main's no-data watchdog.
        //   nullopt = silence is EXPECTED right now and the watchdog must not
        //             apply: external trigger idle, warming up, not streaming.
        // Before the first frame arrives a driver reports the time since it
        // started acquiring, so a device that accepts the subscription and then
        // sends nothing is caught too.
        // Defaulted: a driver that cannot answer is simply never watched.
        virtual std::optional<std::uint64_t> MicrosSinceLastData() const { return std::nullopt; }

        // Shared termination flag (set by the unified main's signal handler)
        std::atomic<bool> &TerminateFlag() { return terminate_; }

    protected:
        void MarkFailed() { failed_.store(true, std::memory_order_release); }

        std::atomic<bool> failed_{false};
        std::atomic<bool> terminate_{false};
    };
} // namespace common
