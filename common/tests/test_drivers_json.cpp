#include "drivers_json.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>


namespace {
    nlohmann::json ReadJson(const std::filesystem::path &path) {
        std::ifstream in(path);
        REQUIRE(in.good());
        return nlohmann::json::parse(in);
    }
} // namespace


TEST_CASE(

    "DriversJson: running document then atomic finalize"
) {
    const auto dir = std::filesystem::temp_directory_path() / "amiga_drivers_json_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "drivers.json";
    std::filesystem::remove(path);

    Common::DriversJson dj(path.string());
    dj.SetRun("20260731_120000", "./recordings", true);
    dj.SetVersion("0.1.0", "abc1234");
    dj.AddDriver("asterx", true, "./asterx_driver/config/config-asterx.yaml");
    dj.AddDriver("gox", false, "./gox_driver/config/config-gox.yaml");
    dj.WriteRunning();

    {
        const auto doc = ReadJson(path);
        CHECK(doc.at("run").at("status") == "running");
        CHECK(doc.at("run").at("timestamp") == "20260731_120000");
        CHECK(doc.at("run").at("output_directory") == "./recordings");
        CHECK(doc.at("run").at("logging_enabled") == true);
        CHECK(doc.at("run").at("version") == "0.1.0");
        CHECK(doc.at("run").at("git_sha") == "abc1234");
        CHECK(doc.at("run").contains("started"));
        CHECK_FALSE(doc.at("run").contains("ended"));
        CHECK(doc.at("drivers").at("asterx").at("enabled") == true);
        CHECK(doc.at("drivers").at("gox").at("enabled") == false);
        CHECK(doc.at("drivers").at("gox").at("config") == "./gox_driver/config/config-gox.yaml");
    }

    dj.Finalize("interrupted (signal 2)");
    {
        const auto doc = ReadJson(path);
        CHECK(doc.at("run").at("status") == "interrupted (signal 2)");
        CHECK(doc.at("run").contains("ended"));
        CHECK(doc.at("run").at("duration_s").get<double>() >= 0.0);
        // The temp file must not linger after the atomic rename.
        CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    }

    std::filesystem::remove_all(dir);
}


TEST_CASE(

    "DriversJson: unwritable path warns but never throws"
) {
    Common::DriversJson dj("/nonexistent-dir/drivers.json");
    dj.SetRun("t", "o", false);
    CHECK_NOTHROW(dj.WriteRunning());
    CHECK_NOTHROW(dj.Finalize("completed"));
}
