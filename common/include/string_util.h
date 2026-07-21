/// @file string_util.h
/// @brief Shared string helpers (header-only). Two split flavors exist on
/// purpose: Split() keeps raw parts including empties (wire/CSV parsing),
/// SplitTrim() trims each part (human-typed command replies).

#ifndef COMMON_STRING_UTIL_H
#define COMMON_STRING_UTIL_H

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>


namespace Common::StringUtil {
	inline std::string Trim(std::string s) {
		const auto not_space = [](unsigned char c) { return !std::isspace(c); };
		s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
		s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
		return s;
	}

	inline std::string ToLower(std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

	// "Front Left" / "front-left" / "FrontLeft" -> "front_left"
	inline std::string ToSnakeCase(std::string_view name) {
		std::string result;
		result.reserve(name.size());
		for (std::size_t i = 0; i < name.size(); ++i) {
			const char ch = name[i];
			if (ch == ' ' || ch == '-') {
				result.push_back('_');
			} else if (std::isupper(static_cast<unsigned char>(ch))) {
				if (i > 0 && name[i - 1] != ' ' && name[i - 1] != '-' && name[i - 1] != '_' &&
					std::islower(static_cast<unsigned char>(name[i - 1]))) {
					result.push_back('_');
				}
				result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
			} else {
				result.push_back(ch);
			}
		}
		return result;
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

	// Standard Base64 (RFC 4648, with padding, no line breaks)
	inline std::string Base64Encode(std::string_view input) {
		static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		out.reserve(((input.size() + 2) / 3) * 4);
		std::size_t i = 0;
		while (i + 3 <= input.size()) {
			const auto a = static_cast<unsigned char>(input[i]), b = static_cast<unsigned char>(input[i + 1]),
					   c = static_cast<unsigned char>(input[i + 2]);
			out.push_back(kAlphabet[a >> 2]);
			out.push_back(kAlphabet[((a & 0x03) << 4) | (b >> 4)]);
			out.push_back(kAlphabet[((b & 0x0F) << 2) | (c >> 6)]);
			out.push_back(kAlphabet[c & 0x3F]);
			i += 3;
		}
		const std::size_t rem = input.size() - i;
		if (rem == 1) {
			const auto a = static_cast<unsigned char>(input[i]);
			out.push_back(kAlphabet[a >> 2]);
			out.push_back(kAlphabet[(a & 0x03) << 4]);
			out.append("==");
		} else if (rem == 2) {
			const auto a = static_cast<unsigned char>(input[i]), b = static_cast<unsigned char>(input[i + 1]);
			out.push_back(kAlphabet[a >> 2]);
			out.push_back(kAlphabet[((a & 0x03) << 4) | (b >> 4)]);
			out.push_back(kAlphabet[(b & 0x0F) << 2]);
			out.push_back('=');
		}
		return out;
	}
}  // namespace Common::StringUtil

#endif	// COMMON_STRING_UTIL_H
