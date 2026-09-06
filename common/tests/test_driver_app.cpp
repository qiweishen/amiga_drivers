/// @file test_driver_app.cpp
/// @brief Interface-conformance test for common::IDriverApp: the unified
/// main's usage pattern (init with predicate → run thread → terminate → join
/// → shutdown) must compile and behave against the base class alone.

#include <doctest/doctest.h>

#include <memory>
#include <thread>

#include "driver_app.h"
#include "thread_util.h"


namespace {
    class FakeDriverApp final : public common::IDriverApp {
    public:
        [[nodiscard]] bool Init(const std::function<bool()> &external_stop = {}) override {
            init_called = true;
            if (external_stop && external_stop()) {
                return false;
            }
            return init_result;
        }

        void Run() override {
            common::ThreadUtil::WaitUntilTerminated(terminate_, std::chrono::milliseconds(1));
            run_finished = true;
        }

        void Shutdown() override { ++shutdown_calls; }

        bool init_result = true;
        bool init_called = false;
        bool run_finished = false;
        int shutdown_calls = 0;
    };
} // namespace


TEST_CASE(

    "IDriverApp drives the unified-main lifecycle pattern"
) {
    std::unique_ptr<common::IDriverApp> app = std::make_unique<FakeDriverApp>();
    auto *fake = static_cast<FakeDriverApp *>(app.get());

    REQUIRE(app->Init());
    CHECK(fake->init_called);
    CHECK_FALSE(app->TerminateFlag().load());

    std::thread t([&app] { app->Run(); });
    app->TerminateFlag().store(true, std::memory_order_release);
    t.join();
    CHECK(fake->run_finished);

    app->Shutdown();
    app->Shutdown(); // idempotence is the derived class's contract; base allows repeats
    CHECK(fake->shutdown_calls == 2);
}


TEST_CASE(

    "IDriverApp init honors the external_stop predicate"
) {
    FakeDriverApp app;
    CHECK_FALSE(app.Init([] { return true; }));
    CHECK(app.Init([] { return false; }));
    CHECK(app.Init()); // default: no predicate
}

TEST_CASE("MicrosSinceLastData defaults to 'not watched' for a driver that does not answer") {
    // The no-data watchdog in main polls every IDriverApp. FakeDriverApp above
    // does NOT override the query — that is the point: the default must compile
    // (a pure virtual would break every implementer that has nothing to report)
    // and must mean "silence carries no information here", never "silent
    // forever", which would abort the run on the first tick.
    const FakeDriverApp app;
    CHECK_FALSE(app.MicrosSinceLastData().has_value());
}

TEST_CASE("A recording failure remains visible after an operator stop and repeated shutdown") {
    FakeDriverApp app;
    app.TerminateFlag().store(true);
    CHECK_FALSE(app.HasFailed());
    app.RequestFailure(); // a final flush failed after stop was requested
    app.Shutdown();
    app.Shutdown();
    CHECK(app.HasFailed());
    CHECK(app.TerminateFlag().load());
}
