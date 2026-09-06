#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "string_util.h"


// Shared YAML schema primitives for every driver config.
// Policy: unknown keys are errors (CheckKeys lists the accepted set), omitted
// keys keep the caller's default, every error names the dotted key path.
// All nodes are taken by const reference: yaml-cpp's non-const operator[]
// inserts keys, and indexing an undefined node throws.
namespace common {
    class ConfigError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    namespace ConfigUtil {
        using Keys = std::initializer_list<std::string_view>;

        inline std::string JoinPath(const std::string &path, std::string_view key) {
            return path.empty() ? std::string(key) : path + "." + std::string(key);
        }

        inline std::string IndexPath(const std::string &path, std::size_t index) {
            return path + "[" + std::to_string(index) + "]";
        }

        [[noreturn]] inline void Fail(const std::string &path, const std::string &what) {
            throw ConfigError(path + ": " + what);
        }

        inline std::string Shown(const YAML::Node &n) {
            if (!n.IsDefined() || n.IsNull()) {
                return "an empty value";
            }
            return n.IsScalar() ? "'" + n.Scalar() + "'" : "a non-scalar value";
        }

        // File / text loading; I/O and syntax errors become ConfigError.
        inline YAML::Node LoadFile(std::string_view path) {
            try {
                return YAML::LoadFile(std::string(path));
            } catch (const std::exception &e) {
                throw ConfigError("failed to load config '" + std::string(path) + "': " + e.what());
            }
        }

        inline YAML::Node LoadText(const std::string &text, const std::string &origin = "<string>") {
            try {
                return YAML::Load(text);
            } catch (const std::exception &e) {
                throw ConfigError("failed to parse config " + origin + ": " + e.what());
            }
        }

        // Undefined / Null count as an empty map. Non-map or foreign keys are errors.
        inline void CheckKeys(const YAML::Node &node, const std::string &path, Keys allowed) {
            if (!node.IsDefined() || node.IsNull()) {
                return;
            }
            if (!node.IsMap()) {
                Fail(path.empty() ? "document" : path, "must be a mapping");
            }
            for (const auto &entry: node) {
                if (!entry.first.IsScalar()) {
                    Fail(path.empty() ? "document" : path, "has a non-scalar key");
                }
                const std::string key = entry.first.Scalar();
                bool known = false;
                for (const auto a: allowed) {
                    if (a == key) {
                        known = true;
                        break;
                    }
                }
                if (known) {
                    continue;
                }
                std::string accepted;
                for (const auto a: allowed) {
                    accepted += (accepted.empty() ? "" : ", ") + std::string(a);
                }
                throw ConfigError("unknown key '" + JoinPath(path, key) + "'; this block accepts only: " + accepted);
            }
        }

        inline bool Present(const YAML::Node &parent, std::string_view key) {
            if (!parent.IsDefined() || !parent.IsMap()) {
                return false;
            }
            return parent[std::string(key)].IsDefined();
        }

        inline YAML::Node Child(const YAML::Node &parent, std::string_view key) {
            if (!parent.IsDefined() || !parent.IsMap()) {
                return YAML::Node(YAML::NodeType::Undefined);
            }
            return parent[std::string(key)];
        }

        inline YAML::Node OptionalMap(const YAML::Node &parent, const std::string &path, std::string_view key,
                                      Keys allowed) {
            const YAML::Node n = Child(parent, key);
            if (!n.IsDefined()) {
                return n;
            }
            if (n.IsNull() || !n.IsMap()) {
                Fail(JoinPath(path, key), "must be a mapping");
            }
            CheckKeys(n, JoinPath(path, key), allowed);
            return n;
        }

        inline YAML::Node RequireMap(const YAML::Node &parent, const std::string &path, std::string_view key,
                                     Keys allowed) {
            const YAML::Node n = Child(parent, key);
            if (!n.IsDefined()) {
                Fail(JoinPath(path, key), "is required");
            }
            return OptionalMap(parent, path, key, allowed);
        }

        inline YAML::Node OptionalSequence(const YAML::Node &parent, const std::string &path, std::string_view key) {
            const YAML::Node n = Child(parent, key);
            if (!n.IsDefined()) {
                return n;
            }
            if (!n.IsSequence()) {
                Fail(JoinPath(path, key), "must be a list (use [] for none)");
            }
            return n;
        }

        inline YAML::Node RequireSequence(const YAML::Node &parent, const std::string &path, std::string_view key) {
            if (!Child(parent, key).IsDefined()) {
                Fail(JoinPath(path, key), "is required");
            }
            return OptionalSequence(parent, path, key);
        }

        namespace detail {
            template<typename T>
            T Convert(const YAML::Node &n, const std::string &full_path) {
                if (!n.IsScalar()) {
                    Fail(full_path, "has an invalid value (" + Shown(n) + ")");
                }
                try {
                    if constexpr (std::is_same_v<T, bool>) {
                        return n.as<bool>();
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        return n.Scalar();
                    } else if constexpr (std::is_floating_point_v<T>) {
                        return static_cast<T>(n.as<double>());
                    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
                        const auto v = n.as<std::int64_t>();
                        if (v < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                            v > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
                            Fail(full_path, "is out of range (" + Shown(n) + ")");
                        }
                        return static_cast<T>(v);
                    } else if constexpr (std::is_integral_v<T>) {
                        const std::string &s = n.Scalar();
                        if (!s.empty() && s.front() == '-') {
                            Fail(full_path, "must not be negative (" + Shown(n) + ")");
                        }
                        const auto v = n.as<std::uint64_t>();
                        if (v > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                            Fail(full_path, "is out of range (" + Shown(n) + ")");
                        }
                        return static_cast<T>(v);
                    } else {
                        return n.as<T>();
                    }
                } catch (const ConfigError &) {
                    throw;
                } catch (const std::exception &) {
                    Fail(full_path, "has an invalid value (" + Shown(n) + ")");
                }
            }
        } // namespace detail

        // Absent -> false, `out` untouched. Null or unconvertible -> ConfigError.
        template<typename T>
        bool Read(const YAML::Node &parent, const std::string &path, std::string_view key, T &out) {
            const YAML::Node n = Child(parent, key);
            if (!n.IsDefined()) {
                return false;
            }
            out = detail::Convert<T>(n, JoinPath(path, key));
            return true;
        }

        template<typename T>
        bool Read(const YAML::Node &parent, const std::string &path, std::string_view key, std::optional<T> &out) {
            T value{};
            if (!Read(parent, path, key, value)) {
                return false;
            }
            out = value;
            return true;
        }

        template<typename T>
        T Require(const YAML::Node &parent, const std::string &path, std::string_view key) {
            T value{};
            if (!Read(parent, path, key, value)) {
                Fail(JoinPath(path, key), "is required");
            }
            return value;
        }

        // Integers are read at full width so an out-of-range value reports the [lo, hi] bounds, not the type's
        template<typename T>
        bool ReadRange(const YAML::Node &parent, const std::string &path, std::string_view key, T lo, T hi, T &out) {
            using Wide = std::conditional_t<std::is_integral_v<T> && !std::is_same_v<T, bool>,
                                            std::conditional_t<std::is_signed_v<T>, std::int64_t, std::uint64_t>, T>;
            Wide value{};
            if (!Read(parent, path, key, value)) {
                return false;
            }
            if (value < static_cast<Wide>(lo) || value > static_cast<Wide>(hi)) {
                Fail(JoinPath(path, key), "must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "] (got " +
                                          std::to_string(value) + ")");
            }
            out = static_cast<T>(value);
            return true;
        }

        template<typename T>
        T RequireRange(const YAML::Node &parent, const std::string &path, std::string_view key, T lo, T hi) {
            T value{};
            if (!ReadRange(parent, path, key, lo, hi, value)) {
                Fail(JoinPath(path, key), "is required");
            }
            return value;
        }

        // Non-blank string.
        inline bool ReadText(const YAML::Node &parent, const std::string &path, std::string_view key,
                             std::string &out) {
            std::string value;
            if (!Read(parent, path, key, value)) {
                return false;
            }
            if (StringUtil::Trim(value).empty()) {
                Fail(JoinPath(path, key), "must not be blank");
            }
            out = value;
            return true;
        }

        inline std::string RequireText(const YAML::Node &parent, const std::string &path, std::string_view key) {
            std::string value;
            if (!ReadText(parent, path, key, value)) {
                Fail(JoinPath(path, key), "is required");
            }
            return value;
        }

        inline std::string AllowedList(Keys allowed) {
            std::string out;
            for (const auto a: allowed) {
                out += (out.empty() ? "" : " | ") + std::string(a);
            }
            return out;
        }

        // Exact-match enumeration (reads the scalar text, so on/off/yes/no stay text).
        inline bool ReadEnum(const YAML::Node &parent, const std::string &path, std::string_view key, Keys allowed,
                             std::string &out) {
            const YAML::Node n = Child(parent, key);
            if (!n.IsDefined()) {
                return false;
            }
            if (!n.IsScalar()) {
                Fail(JoinPath(path, key), "has an invalid value (" + Shown(n) + ")");
            }
            for (const auto a: allowed) {
                if (a == n.Scalar()) {
                    out = n.Scalar();
                    return true;
                }
            }
            Fail(JoinPath(path, key), "invalid value '" + n.Scalar() + "'; allowed: " + AllowedList(allowed));
        }

        template<typename E>
        bool ReadEnum(const YAML::Node &parent, const std::string &path, std::string_view key,
                      std::initializer_list<std::pair<std::string_view, E>> allowed, E &out) {
            const YAML::Node n = Child(parent, key);
            if (!n.IsDefined()) {
                return false;
            }
            if (!n.IsScalar()) {
                Fail(JoinPath(path, key), "has an invalid value (" + Shown(n) + ")");
            }
            std::string names;
            for (const auto &[name, value]: allowed) {
                if (name == n.Scalar()) {
                    out = value;
                    return true;
                }
                names += (names.empty() ? "" : " | ") + std::string(name);
            }
            Fail(JoinPath(path, key), "invalid value '" + n.Scalar() + "'; allowed: " + names);
        }

        // Sequence of scalars. Absent -> false; Null -> error; elements report "<path.key>[i]".
        template<typename T>
        bool ReadSequence(const YAML::Node &parent, const std::string &path, std::string_view key,
                          std::vector<T> &out) {
            const YAML::Node n = OptionalSequence(parent, path, key);
            if (!n.IsDefined()) {
                return false;
            }
            std::vector<T> values;
            std::size_t i = 0;
            for (const auto &item: n) {
                values.push_back(detail::Convert<T>(item, IndexPath(JoinPath(path, key), i++)));
            }
            out = std::move(values);
            return true;
        }
    } // namespace ConfigUtil
} // namespace common
