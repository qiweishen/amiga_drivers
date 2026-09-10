#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "app_config.h"

using namespace lms4xxx;

namespace {
    namespace fs = std::filesystem;

    // A complete, valid document. Tests mutate one thing at a time.
    const char *kValidYaml = R"(
lidar:
    -   id: Front_Left_Laser
        enabled: true
        device:
            ip: "10.95.76.101"
            port: 2111
    -   id: Front_Right_Laser
        enabled: false
        device:
            ip: "10.95.76.103"
scan:
    remission: rssi
ntp:
    enabled: false
    server: "10.95.2.102"
    sync_interval_s: 1
    check_status_s: 5
    lock_timeout_s: 60
    max_time_step_ms: 1000
network:
    recv_buffer_bytes: 4194304
    ring_buffer_frames: 1024
    receive_thread_priority: 99
    receive_thread_cpu: -1
    connect_timeout_ms: 1000
    response_timeout_ms: 1000
    config_timeout_ms: 2000
    tcp_keepalive: true
    keepalive_idle_s: 10
    keepalive_interval_s: 5
    keepalive_count: 3
output:
    queue_max_frames: 512
    max_file_bytes: 1073741824
    chunk_frames: 64
    flush_interval_ms: 1000
    compression_level: 0
    swmr: false
telemetry:
    interval_s: 10
logging:
    stats_interval_s: 1
)";

    // "" = accepted
    std::string LoadError(const std::string &text) {
        try {
            LoadAppConfigText(text);
        } catch (const ConfigError &e) {
            return e.what();
        }
        return "";
    }

    std::string With(const std::string &from, const std::string &to) {
        std::string text = kValidYaml;
        const auto pos = text.find(from);
        REQUIRE(pos != std::string::npos);
        return text.replace(pos, from.size(), to);
    }

    bool Mentions(const std::string &error, const std::string &key) { return error.find(key) != std::string::npos; }
} // namespace


TEST_CASE("The shipped-shape document loads and disabled entries are kept but not enabled") {
    const auto cfg = LoadAppConfigText(kValidYaml);
    REQUIRE(cfg.lidars.size() == 2);
    REQUIRE(cfg.EnabledLidars().size() == 1);
    CHECK(cfg.EnabledLidars()[0]->id == "Front_Left_Laser");
    CHECK(cfg.lidars[0].device.ip == "10.95.76.101");
    CHECK(cfg.lidars[0].device.port == 2111);
    CHECK(cfg.lidars[1].device.port == 2111); // default
    CHECK(cfg.scan.remission == Remission::kRssi);
    CHECK(cfg.telemetry_interval_s == doctest::Approx(10.0));
    CHECK(cfg.output.chunk_frames == 64);
    CHECK(cfg.network.config_timeout_ms == 2000);

    const auto d = cfg.DriverConfigFor(cfg.lidars[0]);
    CHECK(d.name == "Front_Left_Laser");
    CHECK(d.device.ip == "10.95.76.101");
    CHECK(d.network.response_timeout_ms == 1000);
    CHECK_FALSE(d.ntp.enabled);
    CHECK(d.ntp.lock_timeout_s == 60);
    CHECK(d.ntp.max_time_step_ms == 1000);
}

TEST_CASE("The shipped config file itself loads") {
    const auto shipped = fs::path(LMS4XXX_CONFIG_DIR) / "config-lms4xxx.yaml";
    REQUIRE(fs::exists(shipped));
    const auto cfg = LoadAppConfig(shipped.string());
    CHECK(cfg.EnabledLidars().size() >= 1);
    CHECK(cfg.stats_interval_s == doctest::Approx(2.5));
    CHECK(cfg.network.connect_timeout_ms == 500);
    CHECK(cfg.network.response_timeout_ms == 200);
}

TEST_CASE("The v3 layout rejects SWMR at config validation before device startup") {
    CHECK(Mentions(LoadError(With("    swmr: false", "    swmr: true")), "output.swmr"));
}

TEST_CASE("Unknown keys are rejected, block by block, naming the accepted set") {
    SUBCASE("at the root") {
        const auto error = LoadError(std::string(kValidYaml) + "\nrecording:\n    enabled: true\n");
        CHECK(Mentions(error, "unknown key 'recording'"));
        CHECK(Mentions(error, "accepts only: lidar, scan, ntp"));
    }
    SUBCASE("inside scan") {
        const auto error = LoadError(With("scan:\n    remission: rssi",
                                          "scan:\n    remission: rssi\n    angular_resolution_deg: 0.25"));
        CHECK(Mentions(error, "scan.angular_resolution_deg"));
    }
    SUBCASE("inside network") {
        CHECK(Mentions(LoadError(With("    tcp_keepalive: true", "    tcp_keeplive: true")), "network.tcp_keeplive"));
    }
    SUBCASE("inside output: file_prefix is not a key (the file name is fixed)") {
        CHECK(Mentions(LoadError(With("    swmr: false", "    swmr: false\n    file_prefix: lms")),
                       "output.file_prefix"));
    }
    SUBCASE("ntp.role is not a key") {
        CHECK(Mentions(LoadError(With("    enabled: false\n    server", "    enabled: false\n    role: client\n    server")),
                       "ntp.role"));
    }
    SUBCASE("inside a lidar entry") {
        CHECK(Mentions(LoadError(With("    -   id: Front_Left_Laser", "    -   id: Front_Left_Laser\n        name: left")),
                       "lidar[0].name"));
    }
    SUBCASE("inside a lidar device block") {
        CHECK(Mentions(LoadError(With("            ip: \"10.95.76.101\"",
                                      "            ip: \"10.95.76.101\"\n            hostname: lidar1")),
                       "lidar[0].device.hostname"));
    }
    SUBCASE("a per-instance scan block, which is process-wide and would be ignored") {
        CHECK(Mentions(LoadError(With("    -   id: Front_Left_Laser",
                                      "    -   id: Front_Left_Laser\n        scan: { remission: refl }")),
                       "lidar[0].scan"));
    }
}

TEST_CASE("Every numeric bound is enforced and named") {
    SUBCASE("receive_thread_priority is a SCHED_FIFO priority") {
        CHECK(Mentions(LoadError(With("    receive_thread_priority: 99", "    receive_thread_priority: 0")),
                       "network.receive_thread_priority: must be in [1, 99]"));
        CHECK(Mentions(LoadError(With("    receive_thread_priority: 99", "    receive_thread_priority: 100")),
                       "network.receive_thread_priority"));
    }
    SUBCASE("timeouts") {
        CHECK(Mentions(LoadError(With("    response_timeout_ms: 1000", "    response_timeout_ms: 0")),
                       "network.response_timeout_ms"));
        CHECK(Mentions(LoadError(With("    connect_timeout_ms: 1000", "    connect_timeout_ms: 10")),
                       "network.connect_timeout_ms"));
        CHECK(Mentions(LoadError(With("    config_timeout_ms: 2000", "    config_timeout_ms: 99")),
                       "network.config_timeout_ms"));
    }
    SUBCASE("a negative value is refused by name, never wrapped") {
        CHECK(Mentions(LoadError(With("    recv_buffer_bytes: 4194304", "    recv_buffer_bytes: -1")),
                       "network.recv_buffer_bytes: must not be negative"));
    }
    SUBCASE("recv_buffer_bytes / ring_buffer_frames") {
        CHECK(Mentions(LoadError(With("    recv_buffer_bytes: 4194304", "    recv_buffer_bytes: 1024")),
                       "network.recv_buffer_bytes"));
        CHECK(Mentions(LoadError(With("    ring_buffer_frames: 1024", "    ring_buffer_frames: 8")),
                       "network.ring_buffer_frames"));
    }
    SUBCASE("keepalive values must be positive") {
        CHECK(Mentions(LoadError(With("    keepalive_count: 3", "    keepalive_count: 0")), "network.keepalive_count"));
    }
    SUBCASE("output ranges") {
        CHECK(Mentions(LoadError(With("    chunk_frames: 64", "    chunk_frames: 0")), "output.chunk_frames"));
        CHECK(Mentions(LoadError(With("    compression_level: 0", "    compression_level: 10")),
                       "output.compression_level"));
        CHECK(Mentions(LoadError(With("    flush_interval_ms: 1000", "    flush_interval_ms: 0")),
                       "output.flush_interval_ms"));
        CHECK(Mentions(LoadError(With("    queue_max_frames: 512", "    queue_max_frames: 1")), "output.queue_max_frames"));
    }
    SUBCASE("max_file_bytes is 0 or at least a megabyte") {
        CHECK(LoadError(With("    max_file_bytes: 1073741824", "    max_file_bytes: 0")).empty());
        CHECK(Mentions(LoadError(With("    max_file_bytes: 1073741824", "    max_file_bytes: 4096")),
                       "output.max_file_bytes"));
    }
    SUBCASE("telemetry accepts 0 as off") {
        CHECK(LoadError(With("    interval_s: 10", "    interval_s: 0")).empty());
        CHECK(Mentions(LoadError(With("    interval_s: 10", "    interval_s: -1")), "telemetry.interval_s"));
    }
    SUBCASE("ntp intervals") {
        CHECK(Mentions(LoadError(With("    sync_interval_s: 1", "    sync_interval_s: 0")), "ntp.sync_interval_s"));
        CHECK(Mentions(LoadError(With("    check_status_s: 5", "    check_status_s: 3601")), "ntp.check_status_s"));
        CHECK(Mentions(LoadError(With("    lock_timeout_s: 60", "    lock_timeout_s: 0")), "ntp.lock_timeout_s"));
        CHECK(Mentions(LoadError(With("    max_time_step_ms: 1000", "    max_time_step_ms: 60001")),
                       "ntp.max_time_step_ms"));
    }
    SUBCASE("port") {
        CHECK(Mentions(LoadError(With("            port: 2111", "            port: 0")), "lidar[0].device.port"));
    }
}

TEST_CASE("Device addresses are validated and unique") {
    SUBCASE("not an IPv4 address") {
        CHECK(Mentions(LoadError(With("            ip: \"10.95.76.101\"", "            ip: lidar-left.local")),
                       "lidar[0].device.ip"));
        CHECK(Mentions(LoadError(With("            ip: \"10.95.76.101\"", "            ip: \"10.95.76.999\"")),
                       "lidar[0].device.ip"));
    }
    SUBCASE("two enabled entries on the same device") {
        const char *kSameIp = R"(
lidar:
    -   id: A
        device:
            ip: "10.95.76.101"
    -   id: B
        device:
            ip: "10.95.76.101"
)";
        CHECK(Mentions(LoadError(kSameIp), "duplicate device.ip"));
    }
    SUBCASE("a disabled duplicate is not a conflict") {
        const char *kDisabledDuplicate = R"(
lidar:
    -   id: A
        device:
            ip: "10.95.76.101"
    -   id: B
        enabled: false
        device:
            ip: "10.95.76.101"
)";
        CHECK(LoadError(kDisabledDuplicate).empty());
    }
    SUBCASE("duplicate ids") {
        CHECK(Mentions(LoadError(With("    -   id: Front_Right_Laser", "    -   id: Front_Left_Laser")), "duplicate id"));
    }
    SUBCASE("an id that would not survive a file name") {
        CHECK(Mentions(LoadError(With("-   id: Front_Left_Laser", "-   id: Front/Left")), "lidar[0].id"));
    }
}

TEST_CASE("Structural errors are reported, not defaulted") {
    SUBCASE("no lidar block") {
        CHECK(Mentions(LoadError("scan:\n    remission: rssi\n"), "lidar: is required"));
    }
    SUBCASE("every instance disabled") {
        CHECK(Mentions(LoadError(With("        enabled: true", "        enabled: false")), "at least one entry must be enabled"));
    }
    SUBCASE("missing device ip") {
        CHECK(Mentions(LoadError(With("            ip: \"10.95.76.101\"\n", "")), "lidar[0].device.ip: is required"));
    }
    SUBCASE("bad remission value") {
        CHECK(Mentions(LoadError(With("    remission: rssi", "    remission: lux")), "allowed: rssi | refl"));
    }
    SUBCASE("ntp enabled without a server") {
        CHECK(Mentions(LoadError(With("    enabled: false\n    server: \"10.95.2.102\"", "    enabled: true\n    server: \"\"")),
                       "ntp.server"));
    }
    SUBCASE("a null value is not a default") {
        CHECK(Mentions(LoadError(With("    chunk_frames: 64", "    chunk_frames:")), "output.chunk_frames"));
    }
    SUBCASE("a missing file") {
        CHECK_THROWS_AS(LoadAppConfig("/nonexistent/lms.yaml"), ConfigError);
    }
}

TEST_CASE("Omitted optional blocks keep the documented defaults") {
    const auto cfg = LoadAppConfigText(R"(
lidar:
    -   id: Only
        device:
            ip: "192.168.0.1"
)");
    REQUIRE(cfg.lidars.size() == 1);
    CHECK(cfg.lidars[0].device.port == 2111);
    CHECK(cfg.scan.remission == Remission::kRssi);
    CHECK_FALSE(cfg.ntp.enabled);
    CHECK(cfg.output.chunk_frames == 64);
    CHECK(cfg.network.connect_timeout_ms == 500);
    CHECK(cfg.network.config_timeout_ms == 2000);
    CHECK(cfg.stats_interval_s == doctest::Approx(2.5));
}
