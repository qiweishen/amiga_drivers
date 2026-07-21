/// @file test_driver_app.cpp
/// @brief Interface-conformance test for Common::IDriverApp: the unified
/// main's usage pattern (init with predicate → run thread → terminate → join
/// → shutdown) must compile and behave against the base class alone.

#include <doctest/doctest.h>

#include <memory>
#include <thread>

#include "driver_app.h"
#include "thread_util.h"


namespace {
	class FakeDriverApp final : public Common::IDriverApp {
	public:
		[[nodiscard]] bool init(const std::function<bool()> &external_stop = {}) override {
			init_called = true;
			if (external_stop && external_stop()) {
				return false;
			}
			return init_result;
		}

		void run() override {
			Common::ThreadUtil::WaitUntilTerminated(terminate_, std::chrono::milliseconds(1));
			run_finished = true;
		}

		void shutdown() override { ++shutdown_calls; }

		bool init_result = true;
		bool init_called = false;
		bool run_finished = false;
		int shutdown_calls = 0;
	};
}  // namespace


TEST_CASE("IDriverApp drives the unified-main lifecycle pattern") {
	std::unique_ptr<Common::IDriverApp> app = std::make_unique<FakeDriverApp>();
	auto *fake = static_cast<FakeDriverApp *>(app.get());

	REQUIRE(app->init());
	CHECK(fake->init_called);
	CHECK_FALSE(app->TerminateFlag().load());

	std::thread t([&app] { app->run(); });
	app->TerminateFlag().store(true, std::memory_order_release);
	t.join();
	CHECK(fake->run_finished);

	app->shutdown();
	app->shutdown();  // idempotence is the derived class's contract; base allows repeats
	CHECK(fake->shutdown_calls == 2);
}


TEST_CASE("IDriverApp init honors the external_stop predicate") {
	FakeDriverApp app;
	CHECK_FALSE(app.init([] { return true; }));
	CHECK(app.init([] { return false; }));
	CHECK(app.init());	// default: no predicate
}
