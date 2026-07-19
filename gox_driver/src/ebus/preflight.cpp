#include "ebus/preflight.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <fstream>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <set>
#include <sys/types.h>

#include "core/logger.hpp"

namespace jai::ebus {

	namespace {

		struct IfInfo {
			std::string name;
			std::vector<std::string> ips;
		};

		std::vector<IfInfo> list_interfaces() {
			std::vector<IfInfo> out;
			struct ifaddrs *head = nullptr;
			if (::getifaddrs(&head) != 0) {
				return out;
			}
			for (struct ifaddrs *it = head; it != nullptr; it = it->ifa_next) {
				if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET || it->ifa_name == nullptr) {
					continue;
				}
				char buf[INET_ADDRSTRLEN] = {};
				const auto *sin = reinterpret_cast<const struct sockaddr_in *>(it->ifa_addr);
				if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) == nullptr) {
					continue;
				}
				auto found = std::find_if(out.begin(), out.end(), [&](const IfInfo &i) { return i.name == it->ifa_name; });
				if (found == out.end()) {
					out.push_back(IfInfo{ it->ifa_name, { buf } });
				} else {
					found->ips.emplace_back(buf);
				}
			}
			::freeifaddrs(head);
			return out;
		}

		bool read_file_trim(const std::string &path, std::string &out) {
			std::ifstream in(path);
			if (!in) {
				return false;
			}
			std::getline(in, out);
			while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
				out.pop_back();
			}
			return true;
		}

		bool read_file_int(const std::string &path, int64_t &out) {
			std::string s;
			if (!read_file_trim(path, s) || s.empty()) {
				return false;
			}
			try {
				out = std::stoll(s);
			} catch (...) {
				return false;
			}
			return true;
		}

		bool write_file(const std::string &path, const std::string &value) {
			std::ofstream out(path);
			if (!out) {
				return false;
			}
			out << value;
			out.flush();
			return static_cast<bool>(out);
		}

		void add(PreflightReport &r, const std::string &name, CheckLevel level, const std::string &msg) {
			r.checks.push_back(CheckResult{ name, level, msg });
		}

		void check_rp_filter(PreflightReport &report, const std::string &ifname) {
			const std::string path = "/proc/sys/net/ipv4/conf/" + ifname + "/rp_filter";
			const std::string check = "rp_filter(" + ifname + ")";
			int64_t v = 0;
			if (!read_file_int(path, v)) {
				add(report, check, CheckLevel::Info, "cannot read " + path + "; verify rp_filter manually");
				return;
			}
			if (v == 0 || v == 2) {
				add(report, check, CheckLevel::Ok, "rp_filter=" + std::to_string(v));
				return;
			}
			// Strict mode (1) drops GVSP when the camera subnet is asymmetric.
			if (write_file(path, "0")) {
				add(report, check, CheckLevel::Warn,
					"rp_filter was " + std::to_string(v) + "; set to 0 for this run (not persistent)");
				return;
			}
			add(report, check, CheckLevel::Error,
				"rp_filter=" + std::to_string(v) + " (strict) and " + path +
						" is read-only here; run on the host: sysctl -w net.ipv4.conf." + ifname + ".rp_filter=0");
		}

	}  // namespace

	const char *check_level_name(CheckLevel level) {
		switch (level) {
			case CheckLevel::Ok:
				return "ok";
			case CheckLevel::Info:
				return "info";
			case CheckLevel::Warn:
				return "warn";
			case CheckLevel::Error:
				return "error";
		}
		return "unknown";
	}

	bool PreflightReport::has_error() const {
		for (const CheckResult &c: checks) {
			if (c.level == CheckLevel::Error) {
				return true;
			}
		}
		return false;
	}

	nlohmann::ordered_json PreflightReport::to_json() const {
		nlohmann::ordered_json arr = nlohmann::ordered_json::array();
		for (const CheckResult &c: checks) {
			arr.push_back(
					nlohmann::ordered_json{ { "name", c.name }, { "level", check_level_name(c.level) }, { "message", c.message } });
		}
		return arr;
	}

	void PreflightReport::log() const {
		for (const CheckResult &c: checks) {
			const std::string line = "preflight [" + c.name + "] " + c.message;
			switch (c.level) {
				case CheckLevel::Error:
					LOG_ERROR(line);
					break;
				case CheckLevel::Warn:
					LOG_WARN(line);
					break;
				default:
					LOG_INFO(line);
					break;
			}
		}
	}

	PreflightReport run_preflight(const AppConfig &cfg) {
		PreflightReport report;
		const std::vector<IfInfo> interfaces = list_interfaces();

		// Interfaces relevant to the capture: those carrying a configured
		// local_ip; when no camera pins a NIC, every non-loopback interface.
		std::set<std::string> relevant_ifs;
		uint64_t max_rx_request = 0;
		for (const CameraConfig &cam: cfg.cameras) {
			if (!cam.enabled) {
				continue;
			}
			max_rx_request = std::max<uint64_t>(max_rx_request, static_cast<uint64_t>(cam.stream.socket_rx_buffer_mib) * 1024 * 1024);
			if (cam.stream.local_ip.empty()) {
				continue;
			}
			bool found = false;
			for (const IfInfo &i: interfaces) {
				for (const std::string &ip: i.ips) {
					if (ip == cam.stream.local_ip) {
						found = true;
						relevant_ifs.insert(i.name);
					}
				}
			}
			if (found) {
				add(report, "local_ip(" + cam.id + ")", CheckLevel::Ok, cam.stream.local_ip + " found on a local interface");
			} else {
				add(report, "local_ip(" + cam.id + ")", CheckLevel::Error,
					"configured local_ip " + cam.stream.local_ip +
							" is not on any local interface — with a container this usually means it is "
							"not running in the host network namespace (network_mode: host)");
			}
		}
		if (relevant_ifs.empty()) {
			for (const IfInfo &i: interfaces) {
				if (i.name != "lo") {
					relevant_ifs.insert(i.name);
				}
			}
		}

		// rp_filter: "all" plus every relevant interface (the kernel applies
		// max(all, interface)).
		check_rp_filter(report, "all");
		for (const std::string &ifname: relevant_ifs) {
			check_rp_filter(report, ifname);
		}

		// net.core.rmem_max vs the socket receive buffer we will request.
		int64_t rmem_max = 0;
		if (!read_file_int("/proc/sys/net/core/rmem_max", rmem_max)) {
			add(report, "rmem_max", CheckLevel::Info, "cannot read /proc/sys/net/core/rmem_max");
		} else if (static_cast<uint64_t>(rmem_max) < max_rx_request) {
			add(report, "rmem_max", CheckLevel::Warn,
				"net.core.rmem_max=" + std::to_string(rmem_max) + " < requested socket rx buffer " + std::to_string(max_rx_request) +
						"; the kernel will truncate it — run on the host: sysctl -w net.core.rmem_max=" +
						std::to_string(max_rx_request));
		} else {
			add(report, "rmem_max", CheckLevel::Ok,
				"net.core.rmem_max=" + std::to_string(rmem_max) + " >= requested " + std::to_string(max_rx_request));
		}

		// MTU vs explicit packet_size (+28 bytes IP+UDP overhead).
		for (const CameraConfig &cam: cfg.cameras) {
			if (!cam.enabled || cam.stream.packet_size == 0) {
				continue;
			}
			const int64_t needed = static_cast<int64_t>(cam.stream.packet_size) + 28;
			std::set<std::string> ifs_to_check = relevant_ifs;
			if (!cam.stream.local_ip.empty()) {
				ifs_to_check.clear();
				for (const IfInfo &i: interfaces) {
					for (const std::string &ip: i.ips) {
						if (ip == cam.stream.local_ip) {
							ifs_to_check.insert(i.name);
						}
					}
				}
			}
			for (const std::string &ifname: ifs_to_check) {
				int64_t mtu = 0;
				if (!read_file_int("/sys/class/net/" + ifname + "/mtu", mtu)) {
					continue;
				}
				if (mtu < needed) {
					add(report, "mtu(" + cam.id + "," + ifname + ")", CheckLevel::Error,
						"packet_size " + std::to_string(cam.stream.packet_size) + " needs MTU >= " + std::to_string(needed) + " but " +
								ifname + " has MTU " + std::to_string(mtu) +
								"; lower stream.packet_size or raise the MTU (ip "
								"link set " +
								ifname + " mtu 9000)");
				} else {
					add(report, "mtu(" + cam.id + "," + ifname + ")", CheckLevel::Ok,
						ifname + " MTU " + std::to_string(mtu) + " >= " + std::to_string(needed));
				}
			}
		}

		// Firewall: best effort only — just say what must be open.
		add(report, "firewall", CheckLevel::Info,
			"ensure UDP 3956 (GVCP) and the GVSP data port (ephemeral, printed at stream open) are "
			"not filtered by iptables/nftables/ufw on the capture host");

		// eBUS kernel driver: informational; this driver always receives in
		// user mode (SetUserModeSocketRxBufferSize compensates).
		bool module_loaded = false;
		{
			std::ifstream in("/proc/modules");
			std::string line;
			while (std::getline(in, line)) {
				if (line.rfind("ebUniversalProForEthernet", 0) == 0) {
					module_loaded = true;
					break;
				}
			}
		}
		add(report, "kernel_driver", CheckLevel::Info,
			module_loaded ? "ebUniversalProForEthernet kernel module is loaded (receiver=user-mode regardless)"
						  : "ebUniversalProForEthernet kernel module not loaded; receiver=user-mode (expected "
							"in containers)");

		return report;
	}

}  // namespace jai::ebus
