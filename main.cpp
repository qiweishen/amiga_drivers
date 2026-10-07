#include <atomic>
#include <csignal>
#include <iostream>
#include <stdexcept>

#include "acquisition.h"
#include "main_config.h"
#include "signal_handler.h"
#include "utility.h"

int main(int argc, char *argv[]) {
    static std::atomic<bool> terminate{false};
    static std::atomic<int> signal_received{0};
    common::SignalHandler::Install(terminate, signal_received, {SIGINT, SIGTERM, SIGHUP});
    std::signal(SIGPIPE, SIG_IGN);
    try {
        const auto root = common::GetAbsolutePath(common::GetExecutableDir() / "../../");
        auto path = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("config/config-main.yaml");
        if (path.empty()) throw std::invalid_argument("main config path must not be empty");
        path = common::GetAbsolutePath(path.is_relative() ? root / path : path);
        return RunAcquisition(common::LoadMainConfig(path, root), path, terminate, signal_received);
    } catch (const std::exception &error) {
        std::cerr << "Cannot initialize acquisition session: " << error.what() << '\n';
        return 1;
    }
}
