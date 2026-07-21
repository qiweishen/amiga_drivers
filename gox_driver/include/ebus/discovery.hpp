#pragma once

// GigE Vision device discovery and selector matching on top of PvSystem.
// Lifetime invariant: every PvDeviceInfo* is owned by its PvSystem and dies
// with it — a Discovery instance owns the PvSystem and hands out the matched
// PvDeviceInfoGEV*; the caller keeps the Discovery alive until
// PvDeviceGEV::Connect() has returned (cannot dangle, no second Find()).

#include <PvDeviceInfoGEV.h>
#include <PvSystem.h>
#include <memory>
#include <string>
#include <vector>

#include "core/config.hpp"

namespace jai::ebus {

	// Plain-data snapshot of one discovered GEV device (safe to keep after the
	// owning PvSystem is gone).
	struct DiscoveredDevice {
		std::string connection_id;	// PvDeviceInfo::GetConnectionID(), used for Connect-by-string
		std::string ip;
		std::string mac;
		std::string serial;
		std::string model;
		std::string vendor;
		std::string user_name;
		std::string firmware;			  // PvDeviceInfo::GetVersion()
		std::string interface_name;		  // NIC the device was seen on
		bool configuration_valid = true;  // PvDeviceInfoGEV::IsConfigurationValid()
	};

	class Discovery {
	public:
		Discovery();
		~Discovery();

		Discovery(const Discovery &) = delete;
		Discovery &operator=(const Discovery &) = delete;

		// Full PvSystem::Find() with the configured detection timeout and retry
		// schedule, then selector matching (by mac / ip / serial /
		// user_defined_name; MAC compared after jai::normalize_mac on both
		// sides). Throws std::runtime_error on: no match after all retries
		// (after logging every discovered device), more than one match, or an
		// invalid subnet configuration that force_ip could not (or was not
		// allowed to) repair. On success the matched PvDeviceInfoGEV stays valid
		// until this Discovery instance is destroyed.
		DiscoveredDevice find_camera(const SelectorConfig &selector, const DiscoveryConfig &discovery);

		// Matched device info; valid only while this Discovery object lives and
		// only after a successful find_camera().
		const PvDeviceInfoGEV *matched_info() const { return matched_; }

	private:
		struct Found {
			const PvDeviceInfoGEV *info;
			const PvInterface *iface;
		};

		// Runs a fresh Find() and returns all GEV device infos (owned by system_).
		std::vector<Found> find_all(uint32_t timeout_ms);

		std::unique_ptr<PvSystem> system_;
		const PvDeviceInfoGEV *matched_ = nullptr;
	};

	// Snapshot helper (also used by the listing paths).
	DiscoveredDevice describe_device(const PvDeviceInfoGEV *info, const std::string &interface_name);

}  // namespace jai::ebus
