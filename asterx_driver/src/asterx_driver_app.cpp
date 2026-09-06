#include "asterx_driver_app.h"

#include <chrono>
#include <filesystem>

#include <QCoreApplication>
#include <QTimer>
#include <QObject>

#include "app_config.h"
#include "driver_markers.h"
#include "logger.h"
#include "session.h"
#include "thread_util.h"
#include "time_util.h"


namespace {
    // App-level module token: this file's error lines drive the GUI health machine
    common::DriverLog g_log{std::string(common::Markers::kModuleAsterx)};
} // namespace


AsterxDriverApp::AsterxDriverApp(const common::Config &config) {
    config_path_ = config.asterx_config_path;
    data_folder_path_ = config.data_folder_path;
}


AsterxDriverApp::~AsterxDriverApp() {
    Shutdown();
}


AsterxDriverApp::BringUp AsterxDriverApp::BringUpOutcome() {
    std::lock_guard<std::mutex> lk(bring_up_mutex_);
    return bring_up_;
}


void AsterxDriverApp::NotifyBringUp(BringUp outcome) {
    std::lock_guard<std::mutex> lk(bring_up_mutex_);
    if (bring_up_ == BringUp::Pending) {
        bring_up_ = outcome; // first outcome wins; later Configured()/failure notifications are no-ops
    }
    bring_up_cv_.notify_all();
}


void AsterxDriverApp::QtThreadMain(asterx::AppConfig cfg) {
    // At most ONE QCoreApplication per process
    // If another Qt-based driver is ever added, hoist the app object and share
    static int qt_argc = 1; // must outlive the app object (Qt keeps references)
    static char qt_arg0[] = "AmigaDrivers-AsteRx";
    static char *qt_argv[] = {qt_arg0, nullptr};
    QCoreApplication app(qt_argc, qt_argv);

    try {
        asterx::Session session(std::move(cfg)); // ctor throws if the output dir cannot be created
        session.SetLivenessSlot(&data_reference_us_); // outlives the session

        QObject::connect(&session, &asterx::Session::Configured, &session,
                         [this] { NotifyBringUp(BringUp::Recording); });
        QObject::connect(&session, &asterx::Session::FatalError, &session, [this, &app] {
            MarkFailed();
            if (BringUpOutcome() == BringUp::Recording) {
                g_log.Error("{}", common::Markers::kAsterxSessionIssues);
            }
            NotifyBringUp(BringUp::Failed); // no-op if already Recording
            terminate_.store(true, std::memory_order_release);
            app.quit();
        });

        // Forward the external terminate flag into an orderly Qt shutdown;
        // also aborts a still-running bring-up (Session::Shutdown() is safe in any state)
        QTimer stop_poll;
        QObject::connect(&stop_poll, &QTimer::timeout, &stop_poll, [this, &session, &app] {
            if (terminate_.load(std::memory_order_acquire)) {
                session.Shutdown();
                app.quit();
            }
        });
        stop_poll.start(100);

        session.Start();
        app.exec();
        session.Shutdown(); // idempotent: no-op on the fatal path, flush on the quit path
        final_statistics_ = session.FinalStatistics();
        if (session.RecordingIncomplete()) MarkFailed();
    } catch (const std::exception &e) {
        MarkFailed();
        g_log.Error("AsteRx session error: {}", e.what());
    }
    // Driver is dead: unblock Init()/Run(). Same ordering rule: outcome before terminate_.
    // The liveness slot outlives the Session, so it is disarmed here too: the
    // Qt thread is gone and its silence can no longer mean anything.
    data_reference_us_.store(0, std::memory_order_release);
    NotifyBringUp(BringUp::Failed); // no-op unless still Pending (e.g. interrupted bring-up)
    terminate_.store(true, std::memory_order_release);
} // ~Session runs before ~QCoreApplication (stack order) — required by Qt


bool AsterxDriverApp::Init(const std::function<bool()> &external_stop) {
    // Load + validate the driver YAML
    asterx::AppConfig cfg;
    try {
        cfg = asterx::LoadAppConfig(config_path_);
    } catch (const std::exception &e) {
        g_log.Error("AsteRx config error: {}", e.what());
        return false;
    }

    // Unified-mode override: SBF segments go under the unified session folder
    cfg.output_dir = data_folder_path_ / "asterx";

    // Start the Qt worker thread and wait until the receiver reaches Recording or fails
    qt_thread_ = std::thread(&AsterxDriverApp::QtThreadMain, this, std::move(cfg));

    std::unique_lock<std::mutex> lk(bring_up_mutex_);
    while (bring_up_ == BringUp::Pending) {
        if (terminate_.load(std::memory_order_acquire) || (external_stop && external_stop())) {
            break;
        }
        bring_up_cv_.wait_for(lk, std::chrono::milliseconds(100));
    }
    const BringUp outcome = bring_up_;
    lk.unlock();

    if (outcome == BringUp::Recording) {
        g_log.Info("{}", common::Markers::kAsterxInitialized);
        return true;
    }
    if (outcome == BringUp::Failed) {
        if (qt_thread_.joinable()) {
            qt_thread_.join();
        }
        g_log.Error("AsteRx startup failed (receiver unreachable, command rejected, or geometry "
            "verification mismatch)");
        return false;
    }
    // Still Pending: external interrupt during bring-up.
    // The stop-poll timer aborts the bring-up (Session::Shutdown() is safe in any state).
    terminate_.store(true, std::memory_order_release);
    if (qt_thread_.joinable()) {
        qt_thread_.join();
    }
    g_log.Warn("AsteRx bring-up interrupted by shutdown request");
    return false;
}


void AsterxDriverApp::Run() {
    // Block until termination is requested; the Qt thread does the recording.
    common::ThreadUtil::WaitUntilTerminated(terminate_);
}


std::optional<std::uint64_t> AsterxDriverApp::MicrosSinceLastData() const {
    const auto reference = data_reference_us_.load(std::memory_order_acquire);
    if (reference == 0) {
        return std::nullopt;
    }
    const auto now = common::TimeUtil::SteadyNowUs();
    return now > reference ? now - reference : 0;
}


void AsterxDriverApp::Shutdown() {
    data_reference_us_.store(0, std::memory_order_release);
    if (shutdown_called_.exchange(true)) {
        return;
    }

    // Signal termination
    terminate_.store(true, std::memory_order_release);
    if (qt_thread_.joinable()) {
        qt_thread_.join();
    }

    if (HasFailed()) {
        g_log.Error("{}", common::Markers::kAsterxSessionIssues);
    } else {
        g_log.Info("{}", common::Markers::kAsterxShutdown);
    }
}
