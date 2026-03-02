/// @file terminal_spinner.cpp
/// @brief Implementation of TerminalSpinner -- direct-to-stdout animated spinner.
///        All output uses cstdio (fwrite/fflush) to bypass spdlog sinks entirely.

#include "terminal_spinner.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>


namespace {
    // Label displayed alongside every spinner frame.
    constexpr std::string_view kLabel = "Data collecting...";

    // Minimum line width used for trailing-space padding so that shorter frames
    // fully overwrite remnants of longer previous frames.
    constexpr std::size_t kMinPadWidth = 4;

    // Count the number of UTF-8 code points in a string.
    // Each code point typically occupies one display column (correct for ASCII,
    // Latin, Braille, and most symbols). CJK full-width characters would need
    // a more involved width table, but the spinner frames do not use them.
    std::size_t Utf8CodePointCount(std::string_view s) {
        std::size_t count = 0;
        for (unsigned char c: s) {
            // Count only leading bytes: 0xxxxxxx or 11xxxxxx.
            // Continuation bytes (10xxxxxx) are skipped.
            if ((c & 0xC0) != 0x80) {
                ++count;
            }
        }
        return count;
    }
}


TerminalSpinner::TerminalSpinner(std::string_view frames_file_path) {
    std::vector<std::string> raw_frames = LoadFramesFromFile(frames_file_path);
    if (raw_frames.empty()) {
        raw_frames = DefaultFrames();
    }
    BuildFormattedLines(raw_frames);
}


TerminalSpinner::~TerminalSpinner() {
    Clear();
}


void TerminalSpinner::Tick() {
    const std::string &line = frames_[current_frame_];
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    current_frame_ = (current_frame_ + 1) % frames_.size();
}


void TerminalSpinner::Clear() const {
    // Overwrite the spinner line with spaces, then return cursor to column 0.
    // +1 for the leading '\r' character itself occupying the write.
    std::string blank(max_line_length_ + 1, ' ');
    blank[0] = '\r';
    // Terminate with a final '\r' so subsequent output starts at column 0.
    blank.push_back('\r');
    std::fwrite(blank.data(), 1, blank.size(), stdout);
    std::fflush(stdout);
}


std::vector<std::string> TerminalSpinner::LoadFramesFromFile(std::string_view path) {
    std::string path_str{path};
    std::ifstream file{path_str};
    if (!file.is_open()) {
        std::fprintf(stderr, "[TerminalSpinner] Could not open frames file: %.*s, using defaults\n",
                     static_cast<int>(path.size()), path.data());
        return {};
    }

    std::vector<std::string> frames;
    std::string line;
    while (std::getline(file, line)) {
        // Strip trailing whitespace (handles \r\n on Linux if file was edited on Windows).
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        // Skip empty lines and comment lines.
        if (line.empty() || line.front() == '#') {
            continue;
        }
        frames.push_back(std::move(line));
        line = {}; // moved-from string -- reinitialize for next iteration
    }
    return frames;
}


std::vector<std::string> TerminalSpinner::DefaultFrames() {
    return {"|", "/", "-", "\\"};
}


void TerminalSpinner::BuildFormattedLines(const std::vector<std::string> &raw_frames) {
    // Determine the widest raw frame in display columns (code points, not bytes)
    // so we can pad all lines to a uniform visual width.
    std::size_t max_frame_display_width = 0;
    for (const auto &f: raw_frames) {
        max_frame_display_width = std::max(max_frame_display_width, Utf8CodePointCount(f));
    }

    // Visible portion (in display columns): "  <frame><pad>  <label>  "
    // where <pad> right-pads shorter frames to max_frame_display_width.
    const std::size_t visible_width = 2 + max_frame_display_width + 2 + kLabel.size() + kMinPadWidth;

    frames_.clear();
    frames_.reserve(raw_frames.size());

    for (const auto &raw: raw_frames) {
        std::string formatted;
        // Reserve generously: 1 ('\r') + raw bytes + padding + label + extras.
        formatted.reserve(1 + raw.size() + max_frame_display_width + 2 + kLabel.size() + kMinPadWidth + 4);

        formatted.push_back('\r');
        formatted.append("  ");
        formatted.append(raw);
        // Pad frame column to uniform display width using code-point count.
        const std::size_t raw_display_width = Utf8CodePointCount(raw);
        if (raw_display_width < max_frame_display_width) {
            formatted.append(max_frame_display_width - raw_display_width, ' ');
        }
        formatted.append("  ");
        formatted.append(kLabel);
        // Trailing spaces to overwrite any stale characters from a previous longer line.
        formatted.append(kMinPadWidth, ' ');

        frames_.push_back(std::move(formatted));
    }

    // Record max display width for Clear().
    max_line_length_ = visible_width;
}