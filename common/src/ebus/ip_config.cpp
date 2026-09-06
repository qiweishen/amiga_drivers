#include "ebus/ip_config.h"

#include <PvDeviceGEV.h>
#include <PvGenBoolean.h>
#include <PvGenEnum.h>
#include <PvGenInteger.h>
#include <PvGenParameter.h>
#include <PvGenParameterArray.h>
#include <PvGenString.h>
#include <chrono>
#include <thread>

#include "logger.h"
#include "string_util.h"
#include "ebus/sdk_error.h"

namespace common::Ebus {
    namespace {
        common::DriverLog g_log{"eBUS"};

        constexpr uint32_t kReappearPollMs = 1000; // one discovery window while waiting for the new address

        IpState StateOf(const DiscoveredDevice &d) {
            IpState s;
            s.ip = d.ip;
            s.subnet_mask = d.subnet_mask;
            s.gateway = d.gateway;
            s.configuration_valid = d.configuration_valid;
            s.persistent_available = d.persistent_available;
            s.persistent_enabled = d.persistent_enabled;
            s.dhcp_enabled = d.dhcp_enabled;
            return s;
        }


        // First device whose MAC matches, and the adapter that saw it.
        bool find_by_mac(const DiscoveryResult &all, const std::string &want, DiscoveredDevice &device,
                         const HostAdapter *&adapter) {
            for (const HostAdapter &a: all.interfaces) {
                for (const DiscoveredDevice &d: a.devices) {
                    if (common::StringUtil::NormalizeMac(d.mac) == want) {
                        device = d;
                        adapter = &a;
                        return true;
                    }
                }
            }
            return false;
        }


        // GevPersistent* are Integer nodes with an IPv4 representation in SFNC
        // (value = the host-order quad); some XMLs expose them as strings.
        // Dispatch on the node type, read back, report the read-back as text.
        // Returns "" on success, otherwise the reason.
        std::string write_ipv4_node(PvGenParameterArray *params, const std::string &name, uint32_t value,
                                    std::string &readback) {
            PvGenParameter *node = params->Get(PvString(name.c_str()));
            if (node == nullptr) {
                return name + " is not present on this camera";
            }
            if (!node->IsWritable()) {
                return name + " is not writable";
            }
            PvGenType type = PvGenTypeUndefined;
            if (!node->GetType(type).IsOK()) {
                return name + ": GetType failed";
            }
            if (type == PvGenTypeInteger) {
                auto *gi = dynamic_cast<PvGenInteger *>(node);
                if (gi == nullptr) {
                    return name + ": type mismatch (integer)";
                }
                PvResult r = gi->SetValue(static_cast<int64_t>(value));
                if (!r.IsOK()) {
                    return name + " SetValue: " + PvResultToString(r);
                }
                int64_t rb = 0;
                r = gi->GetValue(rb);
                if (!r.IsOK()) {
                    return name + " readback: " + PvResultToString(r);
                }
                readback = FormatIpv4(static_cast<uint32_t>(rb));
                return static_cast<uint32_t>(rb) == value ? "" : name + " reads back " + readback;
            }
            if (type == PvGenTypeString) {
                auto *gs = dynamic_cast<PvGenString *>(node);
                if (gs == nullptr) {
                    return name + ": type mismatch (string)";
                }
                const std::string text = FormatIpv4(value);
                PvResult r = gs->SetValue(PvString(text.c_str()));
                if (!r.IsOK()) {
                    return name + " SetValue: " + PvResultToString(r);
                }
                PvString rb;
                r = gs->GetValue(rb);
                if (!r.IsOK()) {
                    return name + " readback: " + PvResultToString(r);
                }
                readback = ToStd(rb);
                return readback == text ? "" : name + " reads back " + readback;
            }
            return name + " has an unexpected node type";
        }


        // GevCurrentIPConfigurationPersistentIP is a Boolean in SFNC; accept an
        // Integer (0/1) or an Enum ("True"/"False") rendering as well.
        std::string write_bool_node(PvGenParameterArray *params, const std::string &name, bool value,
                                    bool &readback) {
            PvGenParameter *node = params->Get(PvString(name.c_str()));
            if (node == nullptr) {
                return name + " is not present on this camera";
            }
            if (!node->IsWritable()) {
                return name + " is not writable";
            }
            PvGenType type = PvGenTypeUndefined;
            if (!node->GetType(type).IsOK()) {
                return name + ": GetType failed";
            }
            if (type == PvGenTypeBoolean) {
                auto *gb = dynamic_cast<PvGenBoolean *>(node);
                if (gb == nullptr) {
                    return name + ": type mismatch (boolean)";
                }
                PvResult r = gb->SetValue(value);
                if (!r.IsOK()) {
                    return name + " SetValue: " + PvResultToString(r);
                }
                r = gb->GetValue(readback);
                return r.IsOK() ? (readback == value ? "" : name + " did not take") : name + " readback failed";
            }
            if (type == PvGenTypeInteger) {
                auto *gi = dynamic_cast<PvGenInteger *>(node);
                if (gi == nullptr) {
                    return name + ": type mismatch (integer)";
                }
                PvResult r = gi->SetValue(value ? 1 : 0);
                if (!r.IsOK()) {
                    return name + " SetValue: " + PvResultToString(r);
                }
                int64_t rb = 0;
                r = gi->GetValue(rb);
                readback = rb != 0;
                return r.IsOK() ? (readback == value ? "" : name + " did not take") : name + " readback failed";
            }
            if (type == PvGenTypeEnum) {
                auto *ge = dynamic_cast<PvGenEnum *>(node);
                if (ge == nullptr) {
                    return name + ": type mismatch (enum)";
                }
                PvResult r = ge->SetValue(PvString(value ? "True" : "False"));
                if (!r.IsOK()) {
                    r = ge->SetValue(static_cast<int64_t>(value ? 1 : 0));
                }
                if (!r.IsOK()) {
                    return name + " SetValue: " + PvResultToString(r);
                }
                int64_t rb = 0;
                r = ge->GetValue(rb);
                readback = rb != 0;
                return r.IsOK() ? (readback == value ? "" : name + " did not take") : name + " readback failed";
            }
            return name + " has an unexpected node type";
        }


        nlohmann::ordered_json StateJson(const IpState &s) {
            nlohmann::ordered_json j;
            j["ip"] = s.ip;
            j["subnet_mask"] = s.subnet_mask;
            j["gateway"] = s.gateway;
            j["configuration_valid"] = s.configuration_valid;
            j["persistent_available"] = s.persistent_available;
            j["persistent_enabled"] = s.persistent_enabled;
            j["dhcp_enabled"] = s.dhcp_enabled;
            return j;
        }
    } // namespace


    SetIpReport SetDeviceIp(const SetIpRequest &request) {
        SetIpReport r;
        r.request = request;
        const auto fail = [&r](int code, std::string why) {
            r.exit_code = code;
            r.error = std::move(why);
            return r;
        };

        const std::string want = common::StringUtil::NormalizeMac(request.mac);
        if (want.empty()) {
            return fail(kSetIpBadRequest, "\"" + request.mac + "\" is not a MAC address");
        }

        // 1. Find the camera and the adapter that sees it.
        DiscoveryResult found;
        try {
            found = DiscoverDevices(request.discovery_timeout_ms);
        } catch (const std::exception &e) {
            return fail(kSetIpSdkError, std::string("discovery failed: ") + e.what());
        }
        const HostAdapter *adapter = nullptr;
        if (!find_by_mac(found, want, r.device, adapter)) {
            return fail(kSetIpNotFound, "no GigE Vision device with MAC " + request.mac + " answered discovery (" +
                                        std::to_string(found.GetDeviceCount()) + " device(s) seen)");
        }
        r.host_subnets = adapter->addresses;

        // 2. Validate against the host side before touching the camera.
        const std::string invalid = ValidateSetIp(request.ip, request.subnet_mask, request.gateway, r.host_subnets,
                                                    request.allow_foreign_subnet);
        if (!invalid.empty()) {
            return fail(kSetIpBadRequest, invalid);
        }
        uint32_t ip_v = 0, mask_v = 0, gw_v = 0;
        ParseIpv4(request.ip, ip_v);
        ParseIpv4(request.subnet_mask, mask_v);
        ParseIpv4(request.gateway, gw_v);

        // 3. FORCEIP - addressed by MAC in the form the SDK printed it.
        g_log.Info("FORCEIP {} ({}): {} -> {}/{} gw {}", r.device.mac, r.device.model, r.device.ip, request.ip,
                   request.subnet_mask, request.gateway);
        const PvResult forced = PvDeviceGEV::SetIPConfiguration(
            PvString(r.device.mac.c_str()), PvString(request.ip.c_str()), PvString(request.subnet_mask.c_str()),
            PvString(request.gateway.c_str()));
        if (!forced.IsOK()) {
            return fail(kSetIpForceIpRejected, "FORCEIP rejected: " + PvResultToString(forced));
        }
        r.forceip_sent = true;

        // 4. Wait for the camera to announce the new address.
        const auto t0 = std::chrono::steady_clock::now();
        while (true) {
            DiscoveryResult again;
            try {
                again = DiscoverDevices(kReappearPollMs);
            } catch (const std::exception &e) {
                g_log.Warn("discovery while waiting for the new address failed: {}", e.what());
            }
            DiscoveredDevice now;
            const HostAdapter *seen_on = nullptr;
            const auto elapsed_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
            if (find_by_mac(again, want, now, seen_on)) {
                r.after_forceip = StateOf(now);
                if (now.ip == request.ip && now.configuration_valid) {
                    r.reappeared = true;
                    r.reappeared_after_ms = elapsed_ms;
                    r.device.connection_id = now.connection_id;
                    break;
                }
            }
            if (elapsed_ms >= request.reachable_timeout_ms) {
                r.reappeared_after_ms = elapsed_ms;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!r.reappeared) {
            return fail(kSetIpUnreachable,
                        "FORCEIP was sent but the camera did not reappear at " + request.ip + " within " +
                        std::to_string(request.reachable_timeout_ms) +
                        " ms (last seen: " + (r.after_forceip.ip.empty() ? std::string("not at all") : r.after_forceip.ip) +
                        "); a power cycle restores the previous address");
        }
        g_log.Info("{} reachable at {} after {} ms", r.device.mac, request.ip, r.reappeared_after_ms);

        // 5. Persist. Needs control access at the new address.
        r.persistent.attempted = true;
        if (!r.after_forceip.persistent_available) {
            r.persistent.supported = false;
            return fail(kSetIpPersistFailed,
                        "the camera reports no persistent-IP support; the new address is active but transient "
                        "(a power cycle restores the previous one)");
        }
        PvDeviceGEV device;
        const PvResult connected = device.Connect(PvString(request.ip.c_str()), PvAccessControl);
        if (!connected.IsOK()) {
            return fail(kSetIpSdkError, "connect to " + request.ip + " for the persistent write failed: " +
                                        PvResultToString(connected) +
                                        " (the FORCEIP address is active but transient)");
        }
        PvGenParameterArray *params = device.GetParameters();
        std::string error;
        const auto note = [&error](const std::string &e) {
            if (!e.empty()) {
                error += (error.empty() ? "" : "; ") + e;
            }
        };
        // Addresses first, the flag last: some firmwares only accept the flag
        // once the persistent addresses are valid.
        note(write_ipv4_node(params, "GevPersistentIPAddress", ip_v, r.persistent.readback.ip));
        note(write_ipv4_node(params, "GevPersistentSubnetMask", mask_v, r.persistent.readback.subnet_mask));
        note(write_ipv4_node(params, "GevPersistentDefaultGateway", gw_v, r.persistent.readback.gateway));
        bool flag = false;
        note(write_bool_node(params, "GevCurrentIPConfigurationPersistentIP", true, flag));
        r.persistent.readback.persistent_enabled = flag;
        r.persistent.readback.persistent_available = true;
        r.persistent.readback.dhcp_enabled = r.after_forceip.dhcp_enabled;
        r.persistent.readback.configuration_valid = r.after_forceip.configuration_valid;
        device.Disconnect();

        if (!error.empty()) {
            r.persistent.supported = error.find("not present") == std::string::npos;
            r.persistent.error = error;
            return fail(kSetIpPersistFailed, "persistent write failed: " + error +
                                             " - the new address is active but transient (a power cycle restores "
                                             "the previous one)");
        }
        r.persistent.supported = true;
        r.persistent.written = true;
        g_log.Info("{} persistent IP written: {}/{} gw {} (active from the next power-up)", r.device.mac, request.ip,
                   request.subnet_mask, request.gateway);
        return r;
    }


    nlohmann::ordered_json SetIpReportToJson(const SetIpReport &r) {
        nlohmann::ordered_json j;
        j["tool"] = "ebus_set_ip";
        j["exit_code"] = r.exit_code;
        j["error"] = r.error;

        nlohmann::ordered_json req;
        req["mac"] = r.request.mac;
        req["ip"] = r.request.ip;
        req["subnet_mask"] = r.request.subnet_mask;
        req["gateway"] = r.request.gateway;
        req["allow_foreign_subnet"] = r.request.allow_foreign_subnet;
        j["request"] = std::move(req);

        nlohmann::ordered_json dev;
        dev["model"] = r.device.model;
        dev["vendor"] = r.device.vendor;
        dev["serial"] = r.device.serial;
        dev["mac"] = r.device.mac;
        dev["interface"] = r.device.interface_name;
        dev["before"] = r.device.mac.empty() ? nlohmann::ordered_json(nullptr) : StateJson(StateOf(r.device));
        j["device"] = std::move(dev);

        j["host_subnets"] = nlohmann::ordered_json::array();
        for (const HostAddress &h: r.host_subnets) {
            j["host_subnets"].push_back({{"ip", h.ip}, {"subnet_mask", h.subnet_mask}});
        }

        nlohmann::ordered_json force;
        force["sent"] = r.forceip_sent;
        force["reappeared"] = r.reappeared;
        force["reappeared_after_ms"] = r.reappeared_after_ms;
        force["after"] = r.after_forceip.ip.empty() ? nlohmann::ordered_json(nullptr) : StateJson(r.after_forceip);
        j["forceip"] = std::move(force);

        nlohmann::ordered_json persist;
        persist["attempted"] = r.persistent.attempted;
        persist["supported"] = r.persistent.supported;
        persist["written"] = r.persistent.written;
        if (r.persistent.attempted && r.persistent.supported) {
            nlohmann::ordered_json rb;
            rb["ip"] = r.persistent.readback.ip;
            rb["subnet_mask"] = r.persistent.readback.subnet_mask;
            rb["gateway"] = r.persistent.readback.gateway;
            rb["persistent_enabled"] = r.persistent.readback.persistent_enabled;
            persist["readback"] = std::move(rb);
        } else {
            persist["readback"] = nullptr;
        }
        persist["error"] = r.persistent.error;
        persist["note"] = "a persistent address is used from the next power-up; until then "
                "GevIPConfigurationStatus reads ForceIP";
        j["persistent"] = std::move(persist);
        return j;
    }
} // namespace common::Ebus
