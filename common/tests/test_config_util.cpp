#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config_util.h"

using namespace common;
using namespace common::ConfigUtil;

namespace {
    std::string ErrorOf(const std::function<void()> &f) {
        try {
            f();
        } catch (const ConfigError &e) {
            return e.what();
        }
        return "";
    }

    bool Contains(const std::string &s, const std::string &needle) { return s.find(needle) != std::string::npos; }
} // namespace


TEST_CASE("CheckKeys rejects foreign keys and lists the accepted set") {
    const YAML::Node root = LoadText("a: 1\nb: 2\nzz: 3\n");
    const auto err = ErrorOf([&] { CheckKeys(root, "", {"a", "b"}); });
    CHECK(Contains(err, "unknown key 'zz'"));
    CHECK(Contains(err, "accepts only: a, b"));
    CHECK(ErrorOf([&] { CheckKeys(root, "", {"a", "b", "zz"}); }).empty());

    SUBCASE("nested path is dotted") {
        const YAML::Node n = LoadText("outer:\n    inner: {x: 1}\n");
        const auto e = ErrorOf([&] { CheckKeys(n["outer"]["inner"], "outer.inner", {"y"}); });
        CHECK(Contains(e, "unknown key 'outer.inner.x'"));
    }
    SUBCASE("an empty document and a Null block count as an empty map") {
        CHECK(ErrorOf([&] { CheckKeys(LoadText(""), "", {"a"}); }).empty());
        CHECK(ErrorOf([&] { CheckKeys(LoadText("a:\n")["a"], "a", {"x"}); }).empty());
    }
    SUBCASE("a scalar where a mapping is expected") {
        CHECK(Contains(ErrorOf([&] { CheckKeys(LoadText("a: 5\n")["a"], "a", {"x"}); }), "must be a mapping"));
    }
}


TEST_CASE("Read keeps the default when absent and rejects null, blank and bad values") {
    const YAML::Node root = LoadText("i: 7\nnothing:\nblank: \"  \"\ns: hello\nf: 2.5\nb: true\nneg: -3\n");
    int i = 1;
    CHECK(Read(root, "", "i", i));
    CHECK(i == 7);
    CHECK_FALSE(Read(root, "", "missing", i));
    CHECK(i == 7);

    SUBCASE("null value names the key") {
        int v = 0;
        CHECK(Contains(ErrorOf([&] { Read(root, "", "nothing", v); }), "nothing: has an invalid value"));
    }
    SUBCASE("text in a numeric slot") {
        int v = 0;
        CHECK(Contains(ErrorOf([&] { Read(root, "", "s", v); }), "s: has an invalid value ('hello')"));
    }
    SUBCASE("a negative value into an unsigned slot is refused by name, never wrapped") {
        std::uint32_t u = 5;
        CHECK(Contains(ErrorOf([&] { Read(root, "", "neg", u); }), "neg: must not be negative"));
        CHECK(u == 5);
    }
    SUBCASE("range check on the target type") {
        std::uint8_t small = 0;
        CHECK(Contains(ErrorOf([&] { Read(LoadText("v: 300\n"), "", "v", small); }), "v: is out of range"));
        std::int8_t s8 = 0;
        CHECK(Contains(ErrorOf([&] { Read(LoadText("v: -200\n"), "", "v", s8); }), "v: is out of range"));
    }
    SUBCASE("double and bool and string") {
        double f = 0;
        CHECK(Read(root, "", "f", f));
        CHECK(f == doctest::Approx(2.5));
        bool b = false;
        CHECK(Read(root, "", "b", b));
        CHECK(b);
        std::string s;
        CHECK(Read(root, "", "s", s));
        CHECK(s == "hello");
    }
    SUBCASE("optional target") {
        std::optional<double> o;
        CHECK_FALSE(Read(root, "", "missing", o));
        CHECK_FALSE(o.has_value());
        CHECK(Read(root, "", "f", o));
        CHECK(o.value() == doctest::Approx(2.5));
    }
    SUBCASE("ReadText refuses blank") {
        std::string t;
        CHECK(Contains(ErrorOf([&] { ReadText(root, "", "blank", t); }), "blank: must not be blank"));
        CHECK(Contains(ErrorOf([&] { RequireText(root, "", "missing"); }), "missing: is required"));
    }
}


TEST_CASE("Require and ReadRange") {
    const YAML::Node root = LoadText("port: 70000\nok: 5\n");
    CHECK(Require<int>(root, "device", "ok") == 5);
    CHECK(Contains(ErrorOf([&] { Require<int>(root, "device", "nope"); }), "device.nope: is required"));
    int p = 0;
    CHECK(Contains(ErrorOf([&] { ReadRange(root, "device", "port", 1, 65535, p); }),
                   "device.port: must be in [1, 65535] (got 70000)"));
    CHECK(p == 0);
    CHECK(ReadRange(root, "device", "ok", 1, 10, p));
    CHECK(p == 5);
    CHECK(RequireRange(root, "device", "ok", 1, 10) == 5);
}


TEST_CASE("ReadEnum by text and by enum class") {
    enum class Mode { kA, kB };
    const YAML::Node root = LoadText("m: b\nbad: c\nyes_word: on\n");
    std::string text;
    CHECK(ReadEnum(root, "", "m", {"a", "b"}, text));
    CHECK(text == "b");
    CHECK(Contains(ErrorOf([&] { ReadEnum(root, "", "bad", {"a", "b"}, text); }),
                   "bad: invalid value 'c'; allowed: a | b"));
    Mode mode = Mode::kA;
    CHECK(ReadEnum<Mode>(root, "", "m", {{"a", Mode::kA}, {"b", Mode::kB}}, mode));
    CHECK(mode == Mode::kB);
    CHECK_FALSE(ReadEnum<Mode>(root, "", "missing", {{"a", Mode::kA}}, mode));
    // YAML 1.1 booleans stay text for enums
    CHECK(ReadEnum(root, "", "yes_word", {"on", "off"}, text));
    CHECK(text == "on");
}


TEST_CASE("Maps and sequences") {
    const YAML::Node root = LoadText("m: {x: 1}\nnull_map:\nlist: [1, 2, 3]\nnull_list:\nscalar: 3\n");
    CHECK(OptionalMap(root, "", "m", {"x"}).IsMap());
    CHECK_FALSE(OptionalMap(root, "", "absent", {"x"}).IsDefined());
    CHECK(Contains(ErrorOf([&] { OptionalMap(root, "", "null_map", {"x"}); }), "null_map: must be a mapping"));
    CHECK(Contains(ErrorOf([&] { OptionalMap(root, "", "m", {"y"}); }), "unknown key 'm.x'"));
    CHECK(Contains(ErrorOf([&] { RequireMap(root, "", "absent", {"x"}); }), "absent: is required"));

    std::vector<int> v;
    CHECK(ReadSequence(root, "", "list", v));
    CHECK(v == std::vector<int>{1, 2, 3});
    CHECK_FALSE(ReadSequence(root, "", "absent", v));
    CHECK(Contains(ErrorOf([&] { ReadSequence(root, "", "null_list", v); }), "null_list: must be a list"));
    CHECK(Contains(ErrorOf([&] { ReadSequence(root, "", "scalar", v); }), "scalar: must be a list"));
    CHECK(Contains(ErrorOf([&] { RequireSequence(root, "", "absent"); }), "absent: is required"));

    std::vector<int> bad;
    CHECK(Contains(ErrorOf([&] { ReadSequence(LoadText("l: [1, x]\n"), "", "l", bad); }), "l[1]: has an invalid value"));
}


TEST_CASE("LoadFile and LoadText report the origin") {
    CHECK(Contains(ErrorOf([&] { LoadFile("/nonexistent/x.yaml"); }), "failed to load config '/nonexistent/x.yaml'"));
    CHECK(Contains(ErrorOf([&] { LoadText("a: [1,\n", "unit"); }), "failed to parse config unit"));
}
