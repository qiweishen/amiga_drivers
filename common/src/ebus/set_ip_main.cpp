// ebus_set_ip - assign a GigE Vision camera a new IP address and subnet mask
// (web GUI: Camera Tools > Set IP). Two steps, always: FORCEIP by MAC so the
// camera becomes reachable, then connect and write the persistent-IP GenICam
// nodes so the address survives a power cycle. See ebus/ip_config.hpp.
//
// --json prints one JSON document on stdout (on every path, including
// argument errors); the exit code repeats the document's "exit_code":
//   0 ok  2 bad arguments / rejected by validation  3 no such device
//   4 FORCEIP rejected  5 camera did not reappear at the new address
//   6 address active but NOT persisted  7 discovery / connect failure
//
// FORCEIP is a bootstrap command the camera honours even while another
// process holds its control channel: do not run this against a camera that
// is recording. The GUI gates on that; this tool does not.

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "ebus/env_bootstrap.h"
#include "ebus/ip_config.h"

namespace {
    void PrintUsage(const char *argv0) {
        std::printf(
            "Usage: %s --mac <aa:bb:cc:dd:ee:ff> --ip <a.b.c.d> --subnet-mask <m.m.m.m>\n"
            "          [--gateway <g.g.g.g>] [--timeout <ms>] [--reachable-timeout <ms>]\n"
            "          [--allow-foreign-subnet] [--json]\n"
            "\n"
            "Sends a FORCEIP to the camera with that MAC, waits for it to reappear at the\n"
            "new address, then writes GevPersistentIPAddress/SubnetMask/DefaultGateway and\n"
            "enables GevCurrentIPConfigurationPersistentIP so the address survives power\n"
            "cycles. Never run it against a camera that is currently recording.\n"
            "  --gateway            default 0.0.0.0\n"
            "  --timeout            discovery window in ms (default %u)\n"
            "  --reachable-timeout  how long to wait for the new address (default 10000 ms)\n"
            "  --allow-foreign-subnet  skip the check that this host can reach the new address\n"
            "  --json               one JSON document on stdout; exit code = its exit_code\n",
            argv0, common::Ebus::kDiscoveryTimeoutMs);
    }


    bool ParseU32(const char *s, uint32_t &out) {
        if (s == nullptr || *s == '\0') {
            return false;
        }
        char *end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 10);
        if (end == s || *end != '\0' || v == 0 || v > 0xFFFFFFFFul) {
            return false;
        }
        out = static_cast<uint32_t>(v);
        return true;
    }


    int Emit(const common::Ebus::SetIpReport &r, bool json_mode) {
        if (json_mode) {
            std::printf("%s\n", common::Ebus::SetIpReportToJson(r)
                        .dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace).c_str());
            std::fflush(stdout);
            return r.exit_code;
        }
        if (r.exit_code == common::Ebus::kSetIpOk) {
            std::printf("OK: %s (%s) is now at %s/%s gw %s, reachable after %u ms; persistent IP written "
                        "(active from the next power-up)\n",
                        r.device.mac.c_str(), r.device.model.c_str(), r.request.ip.c_str(),
                        r.request.subnet_mask.c_str(), r.request.gateway.c_str(), r.reappeared_after_ms);
        } else {
            std::printf("FAIL %d: %s\n", r.exit_code, r.error.c_str());
        }
        return r.exit_code;
    }
} // namespace

int main(int argc, char **argv) {
    // Logs must never land on stdout: the GUI parses the whole of it as JSON.
    spdlog::set_default_logger(spdlog::stderr_color_mt("ebus_set_ip"));

    // --json first: even an argument error must be reported in the requested shape.
    bool json_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            json_mode = true;
        }
    }

    common::Ebus::SetIpRequest request;
    const auto bad_args = [&](const std::string &why) {
        common::Ebus::SetIpReport r;
        r.request = request;
        r.exit_code = common::Ebus::kSetIpBadRequest;
        r.error = "bad arguments: " + why;
        if (!json_mode) {
            PrintUsage(argv[0]);
        }
        return Emit(r, json_mode);
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--allow-foreign-subnet") {
            request.allow_foreign_subnet = true;
            continue;
        }
        if (i + 1 >= argc) {
            return bad_args(arg + " requires a value");
        }
        const char *val = argv[++i];
        if (arg == "--mac") {
            request.mac = val;
        } else if (arg == "--ip") {
            request.ip = val;
        } else if (arg == "--subnet-mask") {
            request.subnet_mask = val;
        } else if (arg == "--gateway") {
            request.gateway = val;
        } else if (arg == "--timeout") {
            if (!ParseU32(val, request.discovery_timeout_ms)) {
                return bad_args("--timeout must be a positive integer (ms)");
            }
        } else if (arg == "--reachable-timeout") {
            if (!ParseU32(val, request.reachable_timeout_ms)) {
                return bad_args("--reachable-timeout must be a positive integer (ms)");
            }
        } else {
            return bad_args("unknown argument \"" + arg + "\"");
        }
    }
    if (request.mac.empty() || request.ip.empty() || request.subnet_mask.empty()) {
        return bad_args("--mac, --ip and --subnet-mask are required");
    }

    // Must run before the first eBUS SDK call (GenICam environment).
    common::Ebus::BootstrapEnv();
    return Emit(common::Ebus::SetDeviceIp(request), json_mode);
}
