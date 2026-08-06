#include "asterx_driver_app.h"

#include <chrono>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <string_view>

#include <QCoreApplication>
#include <QObject>
#include <QTimer>

#include "app_config.hpp"
#include "driver_markers.h"
#include "logger.h"
#include "session.hpp"
#include "thread_util.h"
#include "utility.h"


namespace {
    // App-level module token: this file's error lines drive the GUI health machine
    Common::DriverLog g_log{std::string(Common::Markers::kModuleAsterx)};
} // namespace


AsterxDriverApp::AsterxDriverApp(const Common::Config &config) {
    // Actual config loading is deferred to init()
    std::filesystem::path exe_dir = Common::GetExecutableDir(); // exe_dir + "../../" -> project root
    config_path_ = exe_dir / "../../" / config.asterx_config_path;
    data_folder_path_ = config.data_folder_path;
}


AsterxDriverApp::~AsterxDriverApp() {
    shutdown();
}


void AsterxDriverApp::NotifyBringUp(BringUp outcome) {
    std::lock_guard<std::mutex> lk(bring_up_mutex_);
    if (bring_up_ == BringUp::Pending) {
        bring_up_ = outcome; // first outcome wins; later configured()/failure notifications are no-ops
    }
    bring_up_cv_.notify_all();
}


void AsterxDriverApp::QtThreadMain(asterx::AppConfig cfg) {
    // At most ONE QCoreApplication per process
    // If another Qt-based driver is ever added, hoist the app object and share
    static int qt_argc = 1; // must outlive the app object (Qt keeps references)
    static char qt_arg0[] = "AmigaDrivers-asterx";
    static char *qt_argv[] = {qt_arg0, nullptr};
    QCoreApplication app(qt_argc, qt_argv);

    try {
        asterx::Session session(std::move(cfg)); // ctor throws if the output dir cannot be created

        QObject::connect(&session, &asterx::Session::configured, &session,
                         [this] { NotifyBringUp(BringUp::Recording); });
        QObject::connect(&session, &asterx::Session::fatalError, &session, [this, &app] {
            // Fatal startup/disk failure (.sbf already flushed): take the rig down.
            // Publish the outcome BEFORE terminate_, or init() could misread the
            // failure as an external interrupt.
            NotifyBringUp(BringUp::Failed); // no-op if already Recording
            terminate_.store(true, std::memory_order_release);
            app.quit();
        });

        // Forward the external terminate flag into an orderly Qt shutdown;
        // also aborts a still-running bring-up (Session::shutdown() is safe in any state)
        QTimer stop_poll;
        QObject::connect(&stop_poll, &QTimer::timeout, &stop_poll, [this, &session, &app] {
            if (terminate_.load(std::memory_order_acquire)) {
                session.shutdown();
                app.quit();
            }
        });
        stop_poll.start(100);

        session.start();
        app.exec();
        session.shutdown(); // idempotent: no-op on the fatal path, flush on the quit path
    } catch (const std::exception &e) {
        g_log.error("AsteRx session error: {}", e.what());
    }
    // Driver is dead: unblock init()/run(). Same ordering rule: outcome before terminate_.
    NotifyBringUp(BringUp::Failed); // no-op unless still Pending (e.g. interrupted bring-up)
    terminate_.store(true, std::memory_order_release);
} // ~Session runs before ~QCoreApplication (stack order) — required by Qt


bool AsterxDriverApp::init(const std::function<bool()> &external_stop) {
    // Load + validate the driver YAML
    asterx::AppConfig cfg;
    try {
        cfg = asterx::load_app_config(config_path_);
    } catch (const std::exception &e) {
        g_log.error("AsteRx config error: {}", e.what());
        return false;
    }

    // Unified-mode override: SBF segments go under the unified session folder
    cfg.output_dir = std::filesystem::path(data_folder_path_) / "bin" / "asterx";

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
        g_log.info("{}", Common::Markers::kAsterxInitialized);
        return true;
    }
    if (outcome == BringUp::Failed) {
        if (qt_thread_.joinable()) {
            qt_thread_.join();
        }
        g_log.error("AsteRx startup failed (receiver unreachable, command rejected, or geometry "
            "verification mismatch)");
        return false;
    }
    // Still Pending: external interrupt during bring-up. The stop-poll timer
    // aborts the bring-up (Session::shutdown() is safe in any state).
    terminate_.store(true, std::memory_order_release);
    if (qt_thread_.joinable()) {
        qt_thread_.join();
    }
    g_log.warn("AsteRx bring-up interrupted by shutdown request");
    return false;
}


void AsterxDriverApp::run() {
    // Block until termination is requested; the Qt thread does the recording.
    // The only internal exit path (Session::fatalError) already sets
    // terminate_, and QtThreadMain sets it again unconditionally on exit.
    Common::ThreadUtil::WaitUntilTerminated(terminate_);
}


void AsterxDriverApp::shutdown() {
    if (shutdown_called_.exchange(true)) {
        return;
    }

    // Signal termination; the Qt thread's stop-poll timer runs
    // Session::shutdown() (flush + close the .sbf) and quits the event loop
    terminate_.store(true, std::memory_order_release);
    if (qt_thread_.joinable()) {
        qt_thread_.join();
    }

    g_log.info("{}", Common::Markers::kAsterxShutdown);
}
