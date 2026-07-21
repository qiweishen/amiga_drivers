/// @file gox_driver_app.h
/// @brief Application wrapper for the JAI Go-X camera driver lifecycle.
///
/// Encapsulates config loading, session bring-up (discovery, PTP, streaming)
/// and recording into an init/run/shutdown pattern matching the INS401/LMS4xxx
/// DriverApp interface. This enables the unified main to run all drivers
/// concurrently. One instance manages every camera listed in the JSON config.

#ifndef GOX_DRIVER_APP_H
#define GOX_DRIVER_APP_H

#include <memory>
#include <string>

#include "data_type.h"
#include "driver_app.h"


// Forward declarations to avoid pulling jai headers into the unified main.
namespace jai {
	class CaptureRunner;
	class StopController;
}  // namespace jai


class GoxDriverApp final : public Common::IDriverApp {
public:
	explicit GoxDriverApp(const Common::Config &config);
	~GoxDriverApp() override;

	// Load the strict-JSON config, route gox logging into the unified logger and bring all
	// enabled cameras up; external_stop/TerminateFlag() abort the minutes-long bring-up
	[[nodiscard]] bool init(const std::function<bool()> &external_stop = {}) override;

	// Blocks in the stats/limits/PTP loop until TerminateFlag() is set
	// externally or an internal stop occurs (error, acquisition limit, low
	// disk); on return TerminateFlag() is always set so the unified main
	// shuts every driver down.
	void run() override;

	// Graceful shutdown: stop all camera sessions, finalize session.json.
	void shutdown() override;

private:
	std::string config_path_;		 // resolved: exe_dir/../../ + gox_config_path
	std::string data_folder_path_;	 // <output>/<timestamp>

	// Declaration order matters: stop_ must outlive runner_ (the runner's
	// CameraSessions hold a StopController*), so it is destroyed last.
	std::unique_ptr<jai::StopController> stop_;
	std::unique_ptr<jai::CaptureRunner> runner_;

	std::atomic<bool> shutdown_called_{ false };
};


#endif	// GOX_DRIVER_APP_H
