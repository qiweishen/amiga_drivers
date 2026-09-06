#include "ebus/discovery.h"

#include <PvDeviceInfoGEV.h>
#include <PvInterface.h>
#include <PvNetworkAdapter.h>
#include <PvSystem.h>
#include <sstream>
#include <stdexcept>

#include "logger.h"
#include "string_util.h"
#include "ebus/sdk_error.h"

namespace common::Ebus {
    namespace {
        common::DriverLog g_log{"eBUS"};

        DiscoveredDevice DescribeDevice(const PvDeviceInfoGEV *info, const std::string &interface_name) {
            DiscoveredDevice d;
            d.connection_id = ToStd(info->GetConnectionID());
            d.display_id = ToStd(info->GetDisplayID());
            d.ip = ToStd(info->GetIPAddress());
            d.subnet_mask = ToStd(info->GetSubnetMask());
            d.gateway = ToStd(info->GetDefaultGateway());
            d.mac = ToStd(info->GetMACAddress());
            d.serial = ToStd(info->GetSerialNumber());
            d.model = ToStd(info->GetModelName());
            d.vendor = ToStd(info->GetVendorName());
            d.user_name = ToStd(info->GetUserDefinedName());
            d.firmware = ToStd(info->GetVersion());
            d.interface_name = interface_name;
            d.configuration_valid = info->IsConfigurationValid();
            d.persistent_available = info->IsPersistentAvailable();
            d.persistent_enabled = info->IsPersistentEnabled();
            d.dhcp_enabled = info->IsDHCPEnabled();
            d.lla_enabled = info->IsLLAEnabled();
            return d;
        }
    } // namespace


    size_t DiscoveryResult::GetDeviceCount() const {
        size_t n = 0;
        for (const HostAdapter &a: interfaces) {
            n += a.devices.size();
        }
        return n;
    }


    std::string DeviceLine(const DiscoveredDevice &d) {
        std::ostringstream os;
        os << d.model << " mac=" << d.mac << " ip=" << d.ip << "/" << d.subnet_mask << " serial=" << d.serial
           << " fw=" << d.firmware;
        if (!d.user_name.empty()) {
            os << " name=\"" << d.user_name << "\"";
        }
        os << " if=" << d.interface_name << (d.configuration_valid ? "" : " [INVALID SUBNET CONFIG]");
        return os.str();
    }


    std::string AdapterLine(const HostAdapter &a) {
        std::ostringstream os;
        os << (a.name.empty() ? "<unknown interface>" : a.name);
        for (const HostAddress &h: a.addresses) {
            os << " " << h.ip << "/" << h.subnet_mask;
        }
        return os.str();
    }


    DiscoveryResult DiscoverDevices(uint32_t timeout_ms) {
        // A fresh PvSystem per pass: the PvDeviceInfo pointers are owned by it
        // and must not outlive it, which is why the result is plain data.
        PvSystem system;
        system.SetDetectionTimeout(timeout_ms);
        CHECK_PV(system.Find(), "PvSystem::Find");

        DiscoveryResult out;
        for (uint32_t i = 0; i < system.GetInterfaceCount(); ++i) {
            const PvInterface *iface = system.GetInterface(i);
            if (iface == nullptr) {
                continue;
            }
            HostAdapter adapter;
            adapter.name = ToStd(iface->GetName());
            if (const auto *nic = dynamic_cast<const PvNetworkAdapter *>(iface)) {
                adapter.mac = ToStd(nic->GetMACAddress());
                for (uint32_t k = 0; k < nic->GetIPAddressCount(); ++k) {
                    adapter.addresses.push_back(HostAddress{ToStd(nic->GetIPAddress(k)), ToStd(nic->GetSubnetMask(k))});
                }
            }
            for (uint32_t j = 0; j < iface->GetDeviceCount(); ++j) {
                const PvDeviceInfo *info = iface->GetDeviceInfo(j);
                if (info == nullptr) {
                    continue;
                }
                if (const auto *gev = dynamic_cast<const PvDeviceInfoGEV *>(info)) {
                    adapter.devices.push_back(DescribeDevice(gev, adapter.name));
                } else {
                    adapter.non_gev_display_ids.push_back(ToStd(info->GetDisplayID()));
                }
            }
            out.interfaces.push_back(std::move(adapter));
        }
        return out;
    }


    DiscoveredDevice FindCamera(const std::string &mac, uint32_t timeout_ms) {
        const std::string want = common::StringUtil::NormalizeMac(mac);
        if (want.empty()) {
            throw std::runtime_error("\"" + mac + "\" is not a MAC address");
        }

        const DiscoveryResult all = DiscoverDevices(timeout_ms);
        struct Match {
            const DiscoveredDevice *device;
            const HostAdapter *adapter;
        };
        std::vector<Match> matches;
        for (const HostAdapter &a: all.interfaces) {
            for (const DiscoveredDevice &d: a.devices) {
                if (common::StringUtil::NormalizeMac(d.mac) == want) {
                    matches.push_back(Match{&d, &a});
                }
            }
        }

        if (matches.size() > 1) {
            std::ostringstream os;
            os << "mac=\"" << mac << "\" matches " << matches.size() << " devices:";
            for (const Match &m: matches) {
                os << "\n  " << DeviceLine(*m.device);
            }
            throw std::runtime_error(os.str());
        }

        if (matches.size() == 1) {
            const DiscoveredDevice &d = *matches.front().device;
            const HostAdapter &a = *matches.front().adapter;
            if (!d.configuration_valid) {
                g_log.Error("Device {} has an invalid IP configuration for its NIC: device {}/{}, NIC {}", d.mac, d.ip,
                            d.subnet_mask, AdapterLine(a));
                throw std::runtime_error(
                    "Device " + d.mac + " is on a foreign subnet (device " + d.ip + "/" + d.subnet_mask + ", NIC " +
                    AdapterLine(a) + "): assign a matching address in the web GUI (Camera Tools > Set IP) or with "
                    "`ebus_set_ip --mac " + d.mac + " --ip <addr> --subnet-mask <mask>`; a persistent write survives "
                    "power cycles, a FORCEIP-only result does not");
            }
            g_log.Trace("Discovered {} Vendor={}", DeviceLine(d), d.vendor);
            return d;
        }

        // No match: list everything we did see, then give up.
        const size_t total = all.GetDeviceCount();
        if (total == 0) {
            g_log.Warn("Discovery: no GigE Vision devices found on any interface");
        } else {
            g_log.Warn("Discovery: no device matches mac=\"{}\"; discovered {} device(s):", mac, total);
            for (const HostAdapter &a: all.interfaces) {
                for (const DiscoveredDevice &d: a.devices) {
                    g_log.Warn("{}", DeviceLine(d));
                }
            }
        }
        throw std::runtime_error("No device with mac \"" + mac + "\" found (" + std::to_string(total) +
                                 " device(s) discovered)");
    }
} // namespace common::Ebus
