// Tests for the factory-default user set helper (src/user_set.cpp).

#include "../include/user_set.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

TEST_CASE("user_set: picks the Default entry in the camera's spelling") {
    const std::vector<std::string> gox{"Default", "User1", "User2", "User3"};
    REQUIRE(gox::PickDefaultUserSetEntry(gox).has_value());
    CHECK(*gox::PickDefaultUserSetEntry(gox) == "Default");

    const std::vector<std::string> upper{"UserSet1", "DEFAULT"};
    REQUIRE(gox::PickDefaultUserSetEntry(upper).has_value());
    CHECK(*gox::PickDefaultUserSetEntry(upper) == "DEFAULT");
}

TEST_CASE("user_set: anything but 'default' is rejected") {
    CHECK_FALSE(gox::PickDefaultUserSetEntry({"UserSet0", "Factory", "UserSetDefault"}).has_value());
    CHECK_FALSE(gox::PickDefaultUserSetEntry({}).has_value());
    CHECK_FALSE(gox::PickDefaultUserSetEntry({"Defaults"}).has_value());
}
