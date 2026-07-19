/// @file gox_driver_app.h
/// @brief Application wrapper for the JAI Go-X camera driver lifecycle.
///
/// Encapsulates config loading, session bring-up (discovery, PTP, streaming)
/// and recording into an init/run/shutdown pattern matching the INS401/LMS4xxx
/// DriverApp interface. This enables the unified main to run all drivers
/// concurrently. One instance manages every camera listed in the JSON config.

#ifndef GOX_DRIVER_APP_H
#define GOX_DRIVER_APP_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "data_type.h"


// Forward declarations to avoid pulling jai headers into the unified main.
namespace jai {
	class CaptureRunner;
	class StopController;
}  // namespace jai


class GoxDriverApp {
public:
	explicit GoxDriverApp(const Common::Config &config);
	~GoxDriverApp();

	// Non-copyable, non-movable.
	GoxDriverApp(const GoxDriverApp &) = delete;
	GoxDriverApp &operator=(const GoxDriverApp &) = delete;
	GoxDriverApp(GoxDriverApp &&) = delete;
	GoxDriverApp &operator=(GoxDriverApp &&) = delete;

	// Load the strict-JSON config, route gox logging into the unified logger,
	// and bring all enabled cameras up (recording into <data_folder>/bin/gox/).
	// Returns false on any fatal error. external_stop and TerminateFlag() are
	// polled during the minutes-long bring-up so a shutdown request aborts it.
	[[nodiscard]] bool init(const std::function<bool()> &external_stop = {});

	// Blocks in the stats/limits/PTP loop until TerminateFlag() is set
	// externally or an internal stop occurs (error, acquisition limit, low
	// disk); on return TerminateFlag() is always set so the unified main
	// shuts every driver down.
	void run();

	// Graceful shutdown: stop all camera sessions, finalize session.json.
	void shutdown();

	// Shared termination flag (set by main.cpp signal handler).
	[[nodiscard]] std::atomic<bool> &TerminateFlag() { return terminate_; }

private:
	std::string config_path_;		 // resolved: exe_dir/../../ + gox_config_path
	std::string data_folder_path_;	 // <output>/<timestamp>

	// Declaration order matters: stop_ must outlive runner_ (the runner's
	// CameraSessions hold a StopController*), so it is destroyed last.
	std::unique_ptr<jai::StopController> stop_;
	std::unique_ptr<jai::CaptureRunner> runner_;

	std::atomic<bool> terminate_{ false };
	std::atomic<bool> shutdown_called_{ false };
};


#endif	// GOX_DRIVER_APP_H
