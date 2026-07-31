#ifndef FX10_DRIVER_APP_H
#define FX10_DRIVER_APP_H

#include <functional>
#include <memory>
#include <string>

#include "data_type.h"
#include "driver_app.h"


class Fx10DriverApp final : public Common::IDriverApp {
public:
    explicit Fx10DriverApp(const Common::Config &config);

    ~Fx10DriverApp() override;

    [[nodiscard]] bool init(const std::function<bool()> &external_stop = {}) override;

    void run() override;

    void shutdown() override;

private:
    struct Impl; // fx10 types live in the .cpp only (keeps eBUS out of main)

    enum class BringUp { kOk, kStopped, kFailed };

    [[nodiscard]] BringUp bringUpSession_(); // connect .. recorder params + disk floor
    [[nodiscard]] bool startStreaming_(); // EnviRecorder::start + StreamReceiver::start
    void monitorLoop_(); // 200 ms stop-condition poll + periodic stats
    void teardownSession_(); // stop -> classify -> finalize -> disconnect
    [[nodiscard]] bool stopRequested_() const;

    void sleepInterruptible_(int total_ms); // 100 ms slices, aborts on stopRequested_()

    std::string config_path_; // resolved: exe_dir/../../ + fx10_config_path
    std::string data_folder_path_; // <output>/<timestamp>
    std::function<bool()> external_stop_;
    std::unique_ptr<Impl> impl_;
    bool init_ok_{false};
    std::atomic<bool> shutdown_called_{false};
};


#endif	// FX10_DRIVER_APP_H
