#include "ebus_driver/block_ids.h"

#include <doctest/doctest.h>
#include <cstdint>
#include <limits>

TEST_CASE("eBUS 16-bit IDs wrap without losing or inventing a frame") {
    ebus_driver::BlockIds ids;
    CHECK_FALSE(ids.Observe(65534).anomaly);
    CHECK(ids.Observe(65535).gap == 0);
    const auto wrapped = ids.Observe(1);
    CHECK_FALSE(wrapped.anomaly);
    CHECK(wrapped.gap == 0);
    CHECK(ids.Observe(2).gap == 0);
}

TEST_CASE("eBUS 16-bit loss across wrap counts only missing valid IDs") {
    ebus_driver::BlockIds ids;
    ids.Observe(65534);
    const auto result = ids.Observe(2); // missing 65535 and 1; zero never exists
    CHECK_FALSE(result.anomaly);
    CHECK(result.gap == 2);
    CHECK(ids.Observe(3).gap == 0);
}

TEST_CASE("eBUS duplicate and backwards IDs preserve the forward baseline") {
    ebus_driver::BlockIds ids;
    ids.Observe(100);
    CHECK(ids.Observe(100).anomaly);
    CHECK(ids.Observe(99).anomaly);
    const auto next = ids.Observe(101);
    CHECK_FALSE(next.anomaly);
    CHECK(next.gap == 0);
    const auto skipped = ids.Observe(104);
    CHECK_FALSE(skipped.anomaly);
    CHECK(skipped.gap == 2);
}

TEST_CASE("eBUS invalid narrow IDs do not seed or replace continuity") {
    ebus_driver::BlockIds ids;
    CHECK(ids.Observe(0).anomaly);
    CHECK(ids.Observe(65536).anomaly);
    CHECK(ids.Observe(7).gap == 0);
    CHECK(ids.Observe(0).anomaly);
    CHECK(ids.Observe(8).gap == 0);
}

TEST_CASE("eBUS 64-bit IDs count gaps without inferring a 16-bit wrap") {
    ebus_driver::BlockIds ids(true);
    ids.Observe(65535);
    CHECK(ids.Observe(65536).gap == 0);
    CHECK(ids.Observe(65540).gap == 3);
    CHECK(ids.Observe(1).anomaly);
    CHECK(ids.Observe(65540).anomaly);
    CHECK(ids.Observe(65541).gap == 0);
}

TEST_CASE("eBUS extended ID arithmetic remains bounded at uint64 maximum") {
    ebus_driver::BlockIds ids(true);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    ids.Observe(maximum - 2);
    const auto result = ids.Observe(maximum);
    CHECK_FALSE(result.anomaly);
    CHECK(result.gap == 1);
    CHECK(ids.Observe(maximum).anomaly);
    CHECK(ids.Observe(1).anomaly);
}
