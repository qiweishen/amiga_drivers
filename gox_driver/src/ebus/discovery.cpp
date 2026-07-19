#include "ebus/discovery.hpp"

#include <PvDeviceGEV.h>
#include <PvInterface.h>
#include <PvNetworkAdapter.h>
#include <chrono>
#include <sstream>
#include <thread>

#include "core/logger.hpp"
#include "core/util.hpp"
#include "ebus/sdk_error.hpp"

namespace jai::ebus {

	namespace {

		std::string nic_description(const PvInterface *iface) {
			if (iface == nullptr) {
				return "<unknown interface>";
			}
			std::ostringstream os;
			os << to_std(iface->GetName());
			const auto *nic = dynamic_cast<const PvNetworkAdapter *>(iface);
			if (nic != nullptr) {
				for (uint32_t i = 0; i < nic->GetIPAddressCount(); ++i) {
					os << " " << to_std(nic->GetIPAddress(i)) << "/" << to_std(nic->GetSubnetMask(i));
				}
			}
			return os.str();
		}

		std::string device_line(const DiscoveredDevice &d) {
			std::ostringstream os;
			os << d.model << " mac=" << d.mac << " ip=" << d.ip << " serial=" << d.serial << " fw=" << d.firmware;
			if (!d.user_name.empty()) {
				os << " name=\"" << d.user_name << "\"";
			}
			os << " if=" << d.interface_name << (d.configuration_valid ? "" : " [INVALID SUBNET CONFIG]");
			return os.str();
		}

		bool selector_matches(const SelectorConfig &sel, const PvDeviceInfoGEV *info) {
			if (sel.by == "mac") {
				return normalize_mac(sel.value) == normalize_mac(to_std(info->GetMACAddress()));
			}
			if (sel.by == "ip") {
				return sel.value == to_std(info->GetIPAddress());
			}
			if (sel.by == "serial") {
				return sel.value == to_std(info->GetSerialNumber());
			}
			if (sel.by == "user_defined_name") {
				return sel.value == to_std(info->GetUserDefinedName());
			}
			return false;
		}

	}  // namespace

	DiscoveredDevice describe_device(const PvDeviceInfoGEV *info, const std::string &interface_name) {
		DiscoveredDevice d;
		d.connection_id = to_std(info->GetConnectionID());
		d.ip = to_std(info->GetIPAddress());
		d.mac = to_std(info->GetMACAddress());
		d.serial = to_std(info->GetSerialNumber());
		d.model = to_std(info->GetModelName());
		d.vendor = to_std(info->GetVendorName());
		d.user_name = to_std(info->GetUserDefinedName());
		d.firmware = to_std(info->GetVersion());
		d.interface_name = interface_name;
		d.configuration_valid = info->IsConfigurationValid();
		return d;
	}

	Discovery::Discovery() = default;
	Discovery::~Discovery() = default;

	std::vector<Discovery::Found> Discovery::find_all(uint32_t timeout_ms) {
		// A fresh PvSystem per pass: previous PvDeviceInfo pointers are dropped
		// first (matched_ is only set right before find_camera returns).
		matched_ = nullptr;
		system_ = std::make_unique<PvSystem>();
		system_->SetDetectionTimeout(timeout_ms);
		CHECK_PV(system_->Find(), "PvSystem::Find");

		std::vector<Found> out;
		for (uint32_t i = 0; i < system_->GetInterfaceCount(); ++i) {
			const PvInterface *iface = system_->GetInterface(i);
			if (iface == nullptr) {
				continue;
			}
			for (uint32_t j = 0; j < iface->GetDeviceCount(); ++j) {
				const PvDeviceInfo *info = iface->GetDeviceInfo(j);
				const auto *gev = dynamic_cast<const PvDeviceInfoGEV *>(info);
				if (gev != nullptr) {
					out.push_back(Found{ gev, iface });
				}
			}
		}
		return out;
	}

	DiscoveredDevice Discovery::find_camera(const SelectorConfig &selector, const DiscoveryConfig &discovery) {
		uint32_t remaining_retries = discovery.retries;
		bool force_ip_sent = false;

		while (true) {
			std::vector<Found> all = find_all(discovery.timeout_ms);
			std::vector<Found> matches;
			for (const Found &f: all) {
				if (selector_matches(selector, f.info)) {
					matches.push_back(f);
				}
			}

			if (matches.size() > 1) {
				std::ostringstream os;
				os << "selector " << selector.by << "=\"" << selector.value << "\" matches " << matches.size() << " devices:";
				for (const Found &f: matches) {
					os << "\n  " << device_line(describe_device(f.info, to_std(f.iface->GetName())));
				}
				throw std::runtime_error(os.str());
			}

			if (matches.size() == 1) {
				const Found &f = matches.front();
				DiscoveredDevice d = describe_device(f.info, to_std(f.iface->GetName()));
				if (!f.info->IsConfigurationValid()) {
					LOG_ERROR("device ", d.mac, " has an invalid IP configuration for its NIC: device ", d.ip, "/",
							  to_std(f.info->GetSubnetMask()), ", NIC ", nic_description(f.iface));
					if (discovery.force_ip.enabled && !force_ip_sent) {
						LOG_WARN("sending FORCEIP to ", d.mac, ": ip=", discovery.force_ip.ip,
								 " mask=", discovery.force_ip.subnet_mask, " gw=", discovery.force_ip.gateway,
								 " (temporary, lost on power cycle)");
						CHECK_PV(PvDeviceGEV::SetIPConfiguration(PvString(d.mac.c_str()), PvString(discovery.force_ip.ip.c_str()),
																 PvString(discovery.force_ip.subnet_mask.c_str()),
																 PvString(discovery.force_ip.gateway.c_str())),
								 "PvDeviceGEV::SetIPConfiguration");
						force_ip_sent = true;
						std::this_thread::sleep_for(std::chrono::milliseconds(1000));
						continue;  // re-Find; does not consume a retry
					}
					throw std::runtime_error("device " + d.mac + " is on the wrong subnet (device " + d.ip + ", NIC " +
											 nic_description(f.iface) +
											 "); fix the NIC/camera addressing or enable discovery.force_ip");
				}
				matched_ = f.info;
				LOG_INFO("discovered ", device_line(d), " vendor=", d.vendor);
				return d;
			}

			// No match: list everything we did see, then retry or give up.
			if (all.empty()) {
				LOG_WARN("discovery: no GigE Vision devices found on any interface");
			} else {
				LOG_WARN("discovery: no device matches ", selector.by, "=\"", selector.value, "\"; discovered ", all.size(),
						 " device(s):");
				for (const Found &f: all) {
					LOG_WARN("  ", device_line(describe_device(f.info, to_std(f.iface->GetName()))));
				}
			}
			if (remaining_retries == 0) {
				throw std::runtime_error("no device matching " + selector.by + "=\"" + selector.value + "\" after " +
										 std::to_string(discovery.retries + 1) + " discovery attempt(s)");
			}
			--remaining_retries;
			std::this_thread::sleep_for(std::chrono::milliseconds(discovery.retry_interval_ms));
		}
	}

}  // namespace jai::ebus
