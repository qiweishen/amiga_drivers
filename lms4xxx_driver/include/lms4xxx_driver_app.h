#ifndef LMS4XXX_DRIVER_APP_H
#define LMS4XXX_DRIVER_APP_H

#include <memory>
#include <string>

#include "driver_app.h"
#include "lms4xxx_data_type.h"
#include "lms4xxx_driver.h"
#include "lms4xxx_scan_record_writer.h"


class Lms4xxxDriverApp final : public Common::IDriverApp {
public:
	explicit Lms4xxxDriverApp(LiDARConfig config);
	~Lms4xxxDriverApp() override;

	// Initialize the driver: load config, create LMS4xxxDriver, connect.
	// The external_stop predicate is unused: connect/configure are bounded by timeouts.
	[[nodiscard]] bool init(const std::function<bool()> &external_stop = {}) override;

	// Run the driver (blocks until terminate_flag is set).
	void run() override;

	// Graceful shutdown: stop scanning, disconnect.
	void shutdown() override;

private:
	struct Impl {
		std::unique_ptr<LMS4xxx::LMS4xxxDriver> driver;
		std::unique_ptr<LMS4xxx::ScanRecordWriter> writer;
		std::string instance_name;	// e.g., "front_left"
	};

	std::unique_ptr<Impl> impl_;
	LiDARConfig config_;
};

#endif	// LMS4XXX_DRIVER_APP_H
