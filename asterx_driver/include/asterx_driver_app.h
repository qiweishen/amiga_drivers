/// @file asterx_driver_app.h
/// @brief Application wrapper for the Septentrio AsteRx GNSS/INS receiver.
///
/// Hosts the driver's Qt event loop (QCoreApplication + Session + SsnRx) on a
/// dedicated worker thread behind the init/run/shutdown pattern matching the
/// INS401/LMS4xxx/GoX DriverApp interface, so the unified main can run all
/// drivers concurrently. The wrapper thread itself never touches a QObject:
/// all cross-thread control is atomic flags + join.

#ifndef ASTERX_DRIVER_APP_H
#define ASTERX_DRIVER_APP_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "data_type.h"
#include "driver_app.h"


// Forward declaration to keep Qt/asterx headers out of the unified main.
namespace asterx {
	struct AppConfig;
}


class AsterxDriverApp final : public Common::IDriverApp {
public:
	explicit AsterxDriverApp(const Common::Config &config);
	~AsterxDriverApp() override;

	[[nodiscard]] bool init(const std::function<bool()> &external_stop = {}) override;

	void run() override;

	void shutdown() override;

private:
	enum class BringUp { Pending, Recording, Failed };

	void QtThreadMain(asterx::AppConfig cfg);  // Qt world lives entirely in here
	void NotifyBringUp(BringUp outcome);	   // Pending -> outcome (first wins), notify_all

	std::string config_path_;		// resolved: exe_dir/../../ + asterx_config_path
	std::string data_folder_path_;	// <output>/<timestamp>

	std::thread qt_thread_;
	std::mutex bring_up_mutex_;
	std::condition_variable bring_up_cv_;
	BringUp bring_up_{ BringUp::Pending };

	std::atomic<bool> shutdown_called_{ false };
};


#endif	// ASTERX_DRIVER_APP_H
