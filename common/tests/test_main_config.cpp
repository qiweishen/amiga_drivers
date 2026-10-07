#include <doctest/doctest.h>

#include <string>

#include "config_util.h"
#include "main_config.h"

namespace {
    common::Config Parse(const std::string &text) {
        return common::ParseMainConfig(common::ConfigUtil::LoadText(text), "/project");
    }

    std::string Error(const std::string &text) {
        try {
            (void) Parse(text);
        } catch (const common::ConfigError &e) {
            return e.what();
        }
        return {};
    }
}

TEST_CASE("Main config: omitted switches never enable a device") {
    const auto cfg = Parse("General: {}\n");
    CHECK_FALSE(cfg.enable_asterx);
    CHECK_FALSE(cfg.enable_fx10);
    CHECK_FALSE(cfg.enable_gox);
    CHECK_FALSE(cfg.enable_lms4xxx);
    CHECK(cfg.operator_name == "Anonymous");
    CHECK(cfg.field_name == "Unknown");
    CHECK(cfg.output_directory == "/project/data");
    CHECK(cfg.asterx_config_path == "/project/asterx_driver/config/config-asterx.yaml");
    CHECK(cfg.fx10_config_path == "/project/fx10_driver/config/config-fx10.yaml");
    CHECK(cfg.gox_config_path == "/project/gox_driver/config/config-gox.yaml");
    CHECK(cfg.lms4xxx_config_path == "/project/lms4xxx_driver/config/config-lms4xxx.yaml");
    CHECK(cfg.sensor_trigger_port.empty());
    CHECK(cfg.guards.disk_min_free_gib == 5.0);
    CHECK(cfg.guards.disk_warn_free_gib == 20.0);
    CHECK(cfg.guards.no_data_warn_s == 5.0);
    CHECK(cfg.guards.no_data_abort_s == 60.0);
}

TEST_CASE("Main config: typoed switches fail instead of falling back") {
    for (const auto *key : {"Enable ASTERX", "Enable FX10", "Enable GOX", "Enable LMS4XXX"}) {
        CAPTURE(key);
        const std::string prefix = std::string("General: {\"") + key + "\": ";
        for (const auto *value : {"tru", "null", "[]", "{}"}) {
            CAPTURE(value);
            CHECK(Error(prefix + value + "}").find(std::string("General.") + key) != std::string::npos);
        }
    }
    const auto cfg = Parse("General: {Enable ASTERX: true, Enable FX10: true, Enable GOX: true, Enable LMS4XXX: true}");
    CHECK(cfg.enable_asterx);
    CHECK(cfg.enable_fx10);
    CHECK(cfg.enable_gox);
    CHECK(cfg.enable_lms4xxx);
}

TEST_CASE("Main config: mappings and supported keys are checked at every level") {
    for (const auto *text : {"", "[]", "null", "{}", "General: null", "General: []",
                             "General: {}\nGuards: null", "General: {}\nSensor Trigger: []"}) {
        CAPTURE(text);
        CHECK_FALSE(Error(text).empty());
    }
    CHECK(Error("General: {}\nGuard: {}").find("unknown key 'Guard'") != std::string::npos);
    CHECK(Error("General: {Enable FX1O: true}").find("General.Enable FX1O") != std::string::npos);
    CHECK(Error("General: {}\nGuards: {No Data Abort: 5}").find("Guards.No Data Abort") != std::string::npos);
    CHECK(Error("General: {}\nSensor Trigger: {Ports: ''}").find("Sensor Trigger.Ports") != std::string::npos);
}

TEST_CASE("Main config: malformed guards are never replaced by defaults") {
    for (const auto *key : {"Disk Min Free GiB", "Disk Warn Free GiB", "No Data Warn S", "No Data Abort S"}) {
        CAPTURE(key);
        for (const auto *value : {"sixty", "null", ".nan", ".inf", "-.inf", "-1", "86401"}) {
            CAPTURE(value);
            const std::string text = std::string("General: {}\nGuards: {\"") + key + "\": " + value + "}";
            CHECK(Error(text).find(std::string("Guards.") + key) != std::string::npos);
        }
    }
    CHECK_FALSE(Error("General: {}\nGuards: {Disk Min Free GiB: 20, Disk Warn Free GiB: 20}").empty());
    CHECK_FALSE(Error("General: {}\nGuards: {No Data Warn S: 60, No Data Abort S: 60}").empty());
    CHECK(Error("General: {}\nGuards: {Disk Min Free GiB: 86400, Disk Warn Free GiB: 0, No Data Warn S: 0, No Data Abort S: 86400}").empty());
    CHECK(Error("General: {}\nGuards: {No Data Warn S: 86400, No Data Abort S: 0}").empty());
}

TEST_CASE("Main config: paths are nonblank and resolved against the project root") {
    const auto cfg = Parse("General: {Output Directory: './captures/../data2', GOX Driver Config Path: /settings/gox.yaml}");
    CHECK(cfg.output_directory == "/project/data2");
    CHECK(cfg.gox_config_path == "/settings/gox.yaml");
    for (const auto *key : {"Output Directory", "ASTERX Driver Config Path", "FX10 Driver Config Path",
                            "GOX Driver Config Path", "LMS4XXX Driver Config Path"}) {
        CAPTURE(key);
        CHECK(Error(std::string("General: {\"") + key + "\": '  '}").find(key) != std::string::npos);
    }
}

TEST_CASE("Main config: an empty trigger port is deliberate but malformed ports fail") {
    CHECK(Parse("General: {}\nSensor Trigger: {Port: ''}").sensor_trigger_port.empty());
    CHECK(Parse("General: {}\nSensor Trigger: {Port: /dev/example}").sensor_trigger_port == "/dev/example");
    for (const auto *value : {"null", "[]", "{}", "'relative/device'", "'  '"}) {
        CAPTURE(value);
        CHECK(Error(std::string("General: {}\nSensor Trigger: {Port: ") + value + "}")
                  .find("Sensor Trigger.Port") != std::string::npos);
    }
}
