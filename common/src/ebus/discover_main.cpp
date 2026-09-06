// ebus_discover - standalone GigE Vision enumeration tool. Lists every
// network adapter PvSystem sees and every GEV device found on it, including
// the subnet-configuration validity flag (the usual reason a camera is
// visible but not connectable) and the device's IP configuration state.
// Serves both camera drivers (GO-X and FX10 are both GigE Vision).
//
// --json emits one machine-readable JSON document on stdout instead of the
// table (consumed by the web GUI). stdout carries ONLY that document: the
// default spdlog logger is redirected to stderr before the first SDK call.

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "ebus/discovery.h"
#include "ebus/env_bootstrap.h"

namespace {
    void PrintUsage(const char *argv0) {
        std::printf("Usage: %s [--timeout <ms>] [--json]\n"
                    "Enumerates GigE Vision devices on all interfaces (default timeout 4000 ms).\n"
                    "  --json  print one JSON document on stdout instead of the table\n",
                    argv0);
    }


    std::string IpConfigText(const common::Ebus::DiscoveredDevice &d) {
        std::string s;
        if (d.persistent_enabled) {
            s += "Persistent";
        }
        if (d.dhcp_enabled) {
            s += (s.empty() ? "" : "+") + std::string("DHCP");
        }
        if (d.lla_enabled) {
            s += (s.empty() ? "" : "+") + std::string("LLA");
        }
        return s.empty() ? "-" : s;
    }
} // namespace

int main(int argc, char **argv) {
    // Logs must never land on stdout: the GUI parses the whole of it as JSON.
    spdlog::set_default_logger(spdlog::stderr_color_mt("ebus_discover"));
    // Must run before the first eBUS SDK call (GenICam environment).
    common::Ebus::BootstrapEnv();

    uint32_t timeout_ms = common::Ebus::kDiscoveryTimeoutMs;
    bool json_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--timeout") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --timeout requires a value in ms\n");
                return 2;
            }
            timeout_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            if (timeout_ms == 0) {
                std::fprintf(stderr, "error: --timeout must be a positive integer\n");
                return 2;
            }
        } else if (arg == "--json") {
            json_mode = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown argument \"%s\"\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (!json_mode) {
        std::printf("Searching for GigE Vision devices (%u ms)...\n", timeout_ms);
    }
    common::Ebus::DiscoveryResult found;
    try {
        found = common::Ebus::DiscoverDevices(timeout_ms);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }

    nlohmann::ordered_json doc;
    doc["timeout_ms"] = timeout_ms;
    doc["interfaces"] = nlohmann::ordered_json::array();

    for (const common::Ebus::HostAdapter &adapter: found.interfaces) {
        nlohmann::ordered_json jiface;
        jiface["name"] = adapter.name;
        jiface["mac"] = adapter.mac;
        jiface["addresses"] = nlohmann::ordered_json::array();
        for (const common::Ebus::HostAddress &h: adapter.addresses) {
            jiface["addresses"].push_back({{"ip", h.ip}, {"subnet_mask", h.subnet_mask}});
        }
        if (!json_mode) {
            std::printf("\nInterface: %s\n", adapter.name.c_str());
            if (!adapter.mac.empty()) {
                std::printf("  MAC %s", adapter.mac.c_str());
                for (const common::Ebus::HostAddress &h: adapter.addresses) {
                    std::printf("  %s/%s", h.ip.c_str(), h.subnet_mask.c_str());
                }
                std::printf("\n");
            }
        }
        jiface["devices"] = nlohmann::ordered_json::array();
        jiface["non_gev"] = adapter.non_gev_display_ids;
        if (adapter.devices.empty() && adapter.non_gev_display_ids.empty()) {
            if (!json_mode) {
                std::printf("  (no devices)\n");
            }
            doc["interfaces"].push_back(std::move(jiface));
            continue;
        }
        if (!json_mode) {
            std::printf("  %-24s %-12s %-18s %-15s %-15s %-14s %-12s %-16s %-14s %s\n", "MODEL", "VENDOR", "MAC",
                        "IP", "MASK", "SERIAL", "FIRMWARE", "USER_NAME", "CONFIG", "IPCFG");
            for (const std::string &id: adapter.non_gev_display_ids) {
                std::printf("  (non-GEV device: %s)\n", id.c_str());
            }
        }
        for (const common::Ebus::DiscoveredDevice &d: adapter.devices) {
            if (!json_mode) {
                std::printf("  %-24s %-12s %-18s %-15s %-15s %-14s %-12s %-16s %-14s %s\n", d.model.c_str(),
                            d.vendor.c_str(), d.mac.c_str(), d.ip.c_str(), d.subnet_mask.c_str(), d.serial.c_str(),
                            d.firmware.c_str(), d.user_name.c_str(),
                            d.configuration_valid ? "valid" : "INVALID-SUBNET", IpConfigText(d).c_str());
            }
            nlohmann::ordered_json jdev;
            jdev["model"] = d.model;
            jdev["vendor"] = d.vendor;
            jdev["mac"] = d.mac;
            jdev["ip"] = d.ip;
            jdev["subnet_mask"] = d.subnet_mask;
            jdev["gateway"] = d.gateway;
            jdev["serial"] = d.serial;
            jdev["firmware"] = d.firmware;
            jdev["user_name"] = d.user_name;
            jdev["config_valid"] = d.configuration_valid;
            jdev["ip_config"] = {
                {"persistent_available", d.persistent_available},
                {"persistent_enabled", d.persistent_enabled},
                {"dhcp_enabled", d.dhcp_enabled},
                {"lla_enabled", d.lla_enabled},
            };
            jiface["devices"].push_back(std::move(jdev));
        }
        doc["interfaces"].push_back(std::move(jiface));
    }
    doc["device_count"] = found.GetDeviceCount();

    if (json_mode) {
        // replace-mode error handler: a device string with invalid UTF-8 must
        // degrade to U+FFFD instead of throwing out of dump()
        std::printf("%s\n", doc.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace).c_str());
    } else {
        std::printf("\n%zu GigE Vision device(s) found.\n", found.GetDeviceCount());
    }
    return 0;
}
