#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "data_type.h"
#include "drivers_json.h"
#include "session_runner.h"

namespace {
    struct RunDirectory {
        std::filesystem::path path;
        RunDirectory() {
            static std::atomic<unsigned> next{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() /
                ("amiga_session_runner_" + std::to_string(stamp) + "_" + std::to_string(next.fetch_add(1)));
            if (!std::filesystem::create_directory(path)) throw std::runtime_error("test directory already exists");
        }
        ~RunDirectory() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    // DriversJson also writes the GUI control document. Keep that document in
    // the fixture directory even when the test process inherited a GUI session.
    struct ScopedStatusFile {
        std::optional<std::string> previous;
        explicit ScopedStatusFile(const std::filesystem::path &path) {
            if (const char *value = std::getenv("AMIGA_STATUS_FILE")) previous = value;
            if (::setenv("AMIGA_STATUS_FILE", path.c_str(), 1) != 0)
                throw std::runtime_error("cannot isolate test status path");
        }
        ~ScopedStatusFile() {
            if (previous) ::setenv("AMIGA_STATUS_FILE", previous->c_str(), 1);
            else ::unsetenv("AMIGA_STATUS_FILE");
        }
    };

    struct Trace {
        std::mutex mutex;
        std::condition_variable started_cv;
        std::vector<std::string> events;
        std::atomic<unsigned> started{0};
        std::atomic<unsigned> exited{0};
        std::atomic<bool> shutdown_before_thread_exit{false};
        std::atomic<bool> wait_timed_out{false};

        void Record(const std::string &event) {
            std::lock_guard lock(mutex);
            events.push_back(event);
        }

        std::vector<std::string> WithPrefix(const std::string &prefix) {
            std::lock_guard lock(mutex);
            std::vector<std::string> out;
            for (const auto &event : events) if (event.starts_with(prefix)) out.push_back(event);
            return out;
        }
    };

    // TLS destruction runs after the worker wrapper has returned, so Shutdown
    // checks actual thread exit rather than merely the last line of Run().
    struct ThreadExit {
        std::shared_ptr<Trace> trace;
        std::string name;
        ~ThreadExit() {
            if (!trace) return;
            trace->Record("thread_exit:" + name);
            trace->exited.fetch_add(1, std::memory_order_release);
        }
    };

    struct Behavior {
        bool init_ok = true;
        bool throw_init = false;
        bool throw_nonstd_init = false;
        bool wait_for_stop = false;
        unsigned return_after_started = 0;
        bool throw_shutdown = false;
        bool throw_statistics = false;
    };

    class FakeDriver final : public common::IDriverApp {
    public:
        FakeDriver(std::string name, std::shared_ptr<Trace> trace, Behavior behavior)
            : name_(std::move(name)), trace_(std::move(trace)), behavior_(behavior) {}

        bool Init(const std::function<bool()> &) override {
            trace_->Record("init:" + name_);
            if (behavior_.throw_nonstd_init) throw 7;
            if (behavior_.throw_init) throw std::runtime_error("fake partial initialization failure");
            return behavior_.init_ok;
        }

        void Run() override {
            thread_local ThreadExit exit;
            exit.trace = trace_;
            exit.name = name_;
            {
                std::lock_guard lock(trace_->mutex);
                trace_->events.push_back("run:" + name_);
                trace_->started.fetch_add(1, std::memory_order_release);
            }
            trace_->started_cv.notify_all();
            if (behavior_.return_after_started != 0) {
                std::unique_lock lock(trace_->mutex);
                if (!trace_->started_cv.wait_for(lock, std::chrono::seconds(2), [&] {
                        return trace_->started.load(std::memory_order_acquire) >= behavior_.return_after_started;
                    })) {
                    trace_->wait_timed_out.store(true);
                    RequestFailure();
                }
            }
            if (behavior_.wait_for_stop) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!terminate_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!terminate_.load(std::memory_order_acquire)) {
                    trace_->wait_timed_out.store(true);
                    RequestFailure(); // bounded failure instead of hanging a broken coordinator test
                }
            }
            trace_->Record("run_return:" + name_);
            // Intentionally do not set TerminateFlag: normal return is owned by
            // the coordinator, not an undocumented convention in each adapter.
        }

        void Shutdown() override {
            if (trace_->exited.load(std::memory_order_acquire) != trace_->started.load(std::memory_order_acquire))
                trace_->shutdown_before_thread_exit.store(true);
            trace_->Record("shutdown:" + name_);
            if (behavior_.throw_shutdown) throw std::runtime_error("fake final close failure");
        }

        nlohmann::ordered_json FinalStatistics() const override {
            trace_->Record("statistics:" + name_);
            if (behavior_.throw_statistics) throw std::runtime_error("fake statistics failure");
            return {{"instance", name_}, {"sentinel", "retained:" + name_}};
        }

    private:
        std::string name_;
        std::shared_ptr<Trace> trace_;
        Behavior behavior_;
    };

    struct Rig {
        RunDirectory directory;
        ScopedStatusFile status_file{directory.path / "control.json"};
        common::DriversJson manifest{(directory.path / "drivers.json").string()};
        common::Config config;
        std::atomic<bool> terminate{false};
        std::atomic<int> signal{0};
        std::shared_ptr<Trace> trace = std::make_shared<Trace>();
        std::vector<common::DriverSlot> drivers;

        Rig() {
            config.data_folder_path = directory.path;
            config.guards = {0.0, 0.0, 0.0, 0.0};
            // No SensorSyncHub, SDK or physical device is constructed.
            manifest.SetRun("test", directory.path.string());
        }

        void Add(const char *name, Behavior behavior = {}) {
            common::DriverSlot slot;
            slot.name = name; // every caller supplies a string literal
            slot.init_failed = "fake init failed";
            slot.run_exception = "fake run failed";
            slot.key = name;
            slot.app = std::make_unique<FakeDriver>(name, trace, behavior);
            drivers.push_back(std::move(slot));
            manifest.AddDriver(name, true, "fake.yaml");
        }

        int Execute() {
            REQUIRE(manifest.WriteRunning());
            return common::RunSession(std::move(drivers), config, manifest, terminate, signal);
        }

        nlohmann::json Document() const {
            std::ifstream in(directory.path / "drivers.json");
            REQUIRE(in.good());
            return nlohmann::json::parse(in);
        }
    };
}

TEST_CASE("SessionRunner rolls back partial initialization and untouched slots in reverse order") {
    for (const int failure_kind : {0, 1, 2}) {
        Rig rig;
        rig.Add("first");
        Behavior failed;
        failed.init_ok = false;
        failed.throw_init = failure_kind == 1;
        failed.throw_nonstd_init = failure_kind == 2;
        rig.Add("partial", failed);
        rig.Add("untouched");
        CHECK(rig.Execute() == 1);
        CHECK((rig.trace->WithPrefix("init:") == std::vector<std::string>{"init:first", "init:partial"}));
        CHECK((rig.trace->WithPrefix("shutdown:") ==
               std::vector<std::string>{"shutdown:untouched", "shutdown:partial", "shutdown:first"}));
        CHECK(rig.trace->started.load() == 0);
        const auto doc = rig.Document();
        REQUIRE(doc.at("driver_results").size() == 3);
        CHECK(doc.at("driver_results").at(1).at("failed") == true);
        CHECK(doc.at("run").at("status").get<std::string>().starts_with("failed"));
    }
}

TEST_CASE("SessionRunner treats normal Run return as rig stop and joins every worker before Shutdown") {
    Rig rig;
    Behavior completes;
    completes.return_after_started = 2;
    rig.Add("completes", completes);
    Behavior waits;
    waits.wait_for_stop = true;
    rig.Add("waits", waits);
    CHECK(rig.Execute() == 0);
    CHECK(rig.trace->started.load() == 2);
    CHECK(rig.trace->exited.load() == 2);
    CHECK_FALSE(rig.trace->wait_timed_out.load());
    CHECK_FALSE(rig.trace->shutdown_before_thread_exit.load());
    CHECK((rig.trace->WithPrefix("shutdown:") == std::vector<std::string>{"shutdown:waits", "shutdown:completes"}));
    CHECK(rig.Document().at("run").at("status") == "completed");
}

TEST_CASE("SessionRunner preserves a Shutdown failure and still closes the remaining drivers") {
    Rig rig;
    rig.Add("first");
    Behavior fails;
    fails.throw_shutdown = true;
    rig.Add("bad_close", fails);
    rig.Add("last");
    CHECK(rig.Execute() == 1);
    CHECK((rig.trace->WithPrefix("shutdown:") ==
           std::vector<std::string>{"shutdown:last", "shutdown:bad_close", "shutdown:first"}));
    CHECK_FALSE(rig.trace->shutdown_before_thread_exit.load());
    const auto doc = rig.Document();
    REQUIRE(doc.at("driver_results").size() == 3);
    CHECK(doc.at("driver_results").at(1).at("failed") == true);
    CHECK(doc.at("driver_results").at(0).at("statistics").at("sentinel") == "retained:first");
    CHECK(doc.at("run").at("status").get<std::string>().starts_with("failed"));
}

TEST_CASE("SessionRunner keeps other final counters when one driver's statistics throws") {
    Rig rig;
    Behavior fails;
    fails.throw_statistics = true;
    rig.Add("bad_statistics", fails);
    rig.Add("retained");
    CHECK(rig.Execute() == 1);
    const auto doc = rig.Document();
    REQUIRE(doc.at("driver_results").size() == 2);
    CHECK(doc.at("driver_results").at(0).at("failed") == true);
    CHECK(doc.at("driver_results").at(0).at("statistics").contains("statistics_error"));
    CHECK(doc.at("driver_results").at(1).at("statistics").at("sentinel") == "retained:retained");
    CHECK(doc.at("run").at("status").get<std::string>().starts_with("failed"));
}
