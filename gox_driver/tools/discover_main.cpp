// jai_discover — standalone GigE Vision enumeration tool. Lists every
// network adapter PvSystem sees and every GEV device found on it, including
// the subnet-configuration validity flag (the usual reason a camera is
// visible but not connectable).
//
// --json emits one machine-readable JSON document on stdout instead of the
// table (consumed by the web GUI); without the flag the output is unchanged.

#include "ebus/env_bootstrap.hpp"
#include "ebus/sdk_error.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include <PvDeviceInfoGEV.h>
#include <PvInterface.h>
#include <PvNetworkAdapter.h>
#include <PvSystem.h>

namespace {

void print_usage(const char* argv0) {
    std::printf("Usage: %s [--timeout <ms>] [--json]\n"
                "Enumerates GigE Vision devices on all interfaces (default timeout 4000 ms).\n"
                "  --json  print one JSON document on stdout instead of the table\n",
                argv0);
}

} // namespace

int main(int argc, char** argv) {
    // Must run before the first eBUS SDK call (GenICam environment).
    jai::ebus::bootstrap_env();

    uint32_t timeout_ms = 4000;
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
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown argument \"%s\"\n", arg.c_str());
            print_usage(argv[0]);
            return 2;
        }
    }

    PvSystem system;
    system.SetDetectionTimeout(timeout_ms);
    if (!json_mode) {
        std::printf("Searching for GigE Vision devices (%u ms)...\n", timeout_ms);
    }
    const PvResult result = system.Find();
    if (!result.IsOK()) {
        std::fprintf(stderr, "PvSystem::Find failed: %s\n",
                     jai::ebus::pv_result_to_string(result).c_str());
        return 1;
    }

    nlohmann::ordered_json doc;
    doc["timeout_ms"] = timeout_ms;
    doc["interfaces"] = nlohmann::ordered_json::array();

    uint32_t total_devices = 0;
    for (uint32_t i = 0; i < system.GetInterfaceCount(); ++i) {
        const PvInterface* iface = system.GetInterface(i);
        if (iface == nullptr) {
            continue;
        }
        nlohmann::ordered_json jiface;
        jiface["name"] = jai::ebus::to_std(iface->GetName());
        if (!json_mode) {
            std::printf("\nInterface: %s\n", jai::ebus::to_std(iface->GetName()).c_str());
        }
        const auto* nic = dynamic_cast<const PvNetworkAdapter*>(iface);
        if (nic != nullptr) {
            if (!json_mode) {
                std::printf("  MAC %s", jai::ebus::to_std(nic->GetMACAddress()).c_str());
            }
            jiface["mac"] = jai::ebus::to_std(nic->GetMACAddress());
            jiface["addresses"] = nlohmann::ordered_json::array();
            for (uint32_t k = 0; k < nic->GetIPAddressCount(); ++k) {
                if (!json_mode) {
                    std::printf("  %s/%s", jai::ebus::to_std(nic->GetIPAddress(k)).c_str(),
                                jai::ebus::to_std(nic->GetSubnetMask(k)).c_str());
                }
                jiface["addresses"].push_back(
                    {{"ip", jai::ebus::to_std(nic->GetIPAddress(k))},
                     {"subnet_mask", jai::ebus::to_std(nic->GetSubnetMask(k))}});
            }
            if (!json_mode) {
                std::printf("\n");
            }
        }
        jiface["devices"] = nlohmann::ordered_json::array();
        if (iface->GetDeviceCount() == 0) {
            if (!json_mode) {
                std::printf("  (no devices)\n");
            }
            doc["interfaces"].push_back(std::move(jiface));
            continue;
        }
        if (!json_mode) {
            std::printf("  %-24s %-12s %-18s %-15s %-14s %-12s %-16s %s\n", "MODEL", "VENDOR",
                        "MAC", "IP", "SERIAL", "FIRMWARE", "USER_NAME", "CONFIG");
        }
        for (uint32_t j = 0; j < iface->GetDeviceCount(); ++j) {
            const PvDeviceInfo* info = iface->GetDeviceInfo(j);
            const auto* gev = dynamic_cast<const PvDeviceInfoGEV*>(info);
            if (gev == nullptr) {
                // Non-GEV devices carry none of the fields below; table-only.
                if (!json_mode) {
                    std::printf("  (non-GEV device: %s)\n",
                                info != nullptr ? jai::ebus::to_std(info->GetDisplayID()).c_str()
                                                : "?");
                }
                continue;
            }
            ++total_devices;
            if (!json_mode) {
                std::printf("  %-24s %-12s %-18s %-15s %-14s %-12s %-16s %s\n",
                            jai::ebus::to_std(gev->GetModelName()).c_str(),
                            jai::ebus::to_std(gev->GetVendorName()).c_str(),
                            jai::ebus::to_std(gev->GetMACAddress()).c_str(),
                            jai::ebus::to_std(gev->GetIPAddress()).c_str(),
                            jai::ebus::to_std(gev->GetSerialNumber()).c_str(),
                            jai::ebus::to_std(gev->GetVersion()).c_str(),
                            jai::ebus::to_std(gev->GetUserDefinedName()).c_str(),
                            gev->IsConfigurationValid() ? "valid" : "INVALID-SUBNET");
            }
            jiface["devices"].push_back(
                {{"model", jai::ebus::to_std(gev->GetModelName())},
                 {"vendor", jai::ebus::to_std(gev->GetVendorName())},
                 {"mac", jai::ebus::to_std(gev->GetMACAddress())},
                 {"ip", jai::ebus::to_std(gev->GetIPAddress())},
                 {"serial", jai::ebus::to_std(gev->GetSerialNumber())},
                 {"firmware", jai::ebus::to_std(gev->GetVersion())},
                 {"user_name", jai::ebus::to_std(gev->GetUserDefinedName())},
                 {"config_valid", gev->IsConfigurationValid()}});
        }
        doc["interfaces"].push_back(std::move(jiface));
    }
    doc["device_count"] = total_devices;

    if (json_mode) {
        // replace-mode error handler: a device string with invalid UTF-8 must
        // degrade to U+FFFD instead of throwing out of dump()
        std::printf("%s\n",
                    doc.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace).c_str());
    } else {
        std::printf("\n%u GigE Vision device(s) found.\n", total_devices);
    }
    return 0;
}
