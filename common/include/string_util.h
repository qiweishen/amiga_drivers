#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>


namespace common::StringUtil {
    inline std::string Trim(std::string s) {
        const auto not_space = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return s;
    }

    inline std::string ToLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    inline bool EqualsCi(const std::string &a, const std::string &b) {
        return ToLower(a) == ToLower(b);
    }

    inline bool StartsWithCi(const std::string &s, const std::string &prefix) {
        if (s.size() < prefix.size()) {
            return false;
        }
        return ToLower(s.substr(0, prefix.size())) == ToLower(prefix);
    }

    // Raw split: keeps every part verbatim, including empty ones
    inline std::vector<std::string> Split(std::string_view str, char delimiter) {
        std::vector<std::string> tokens;
        std::size_t start = 0;
        std::size_t end = str.find(delimiter);
        while (end != std::string_view::npos) {
            tokens.emplace_back(str.substr(start, end - start));
            start = end + 1;
            end = str.find(delimiter, start);
        }
        if (start <= str.size()) {
            tokens.emplace_back(str.substr(start));
        }
        return tokens;
    }

    // Split with each part whitespace-trimmed. getline semantics: a trailing
    // separator does NOT produce a final empty part, and "" yields no parts
    inline std::vector<std::string> SplitTrim(const std::string &s, char sep) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, sep)) {
            out.push_back(Trim(part));
        }
        return out;
    }

    inline std::string Join(const std::vector<std::string> &parts, char sep) {
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) {
                out.push_back(sep);
            }
            out.append(parts[i]);
        }
        return out;
    }

    // "00:0C:DF:12:34:56" / "00-0c-df-12-34-56" / "000cdf123456" -> lowercase
    // 12-hex-digit form; "" when the input is not a MAC address
    inline std::string NormalizeMac(const std::string &mac) {
        std::string out;
        out.reserve(12);
        for (char c: mac) {
            if (c == ':' || c == '-' || c == '.') {
                continue;
            }
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                return {};
            }
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out.size() == 12 ? out : std::string{};
    }

    // Compact a possibly multi-line string for single-line logging
    inline std::string OneLine(std::string s) {
        for (char &c: s) {
            if (c == '\r' || c == '\n' || c == '\t') {
                c = ' ';
            }
        }
        return s;
    }
} // namespace common::StringUtil
