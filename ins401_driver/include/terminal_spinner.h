/// @file terminal_spinner.h
/// @brief Terminal spinner widget for visual feedback during system operation.
///        Renders animated frames directly to stdout using carriage-return overwrite.
///        Intentionally avoids spdlog to prevent spinner output from polluting log files.

#ifndef TERMINAL_SPINNER_H
#define TERMINAL_SPINNER_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>


class TerminalSpinner {
public:
    // Construct spinner. Loads frames from the given file path.
    // If file cannot be read or is empty, falls back to a built-in default set.
    explicit TerminalSpinner(std::string_view frames_file_path);

    ~TerminalSpinner();

    // Non-copyable: the spinner owns exclusive cursor-line state on stdout.
    TerminalSpinner(const TerminalSpinner &) = delete;

    TerminalSpinner &operator=(const TerminalSpinner &) = delete;

    // Advance to the next frame and render to terminal.
    // Called from the main loop (~5 Hz / every 200 ms). Must be fast.
    // Writes directly to stdout with carriage-return overwrite; no allocations.
    void Tick();

    // Clear the spinner line from terminal (restore clean state).
    // Call before shutdown or before printing final log messages.
    void Clear() const;

private:
    // Pre-formatted frame strings ready for direct fwrite to stdout.
    // Each entry contains the full line including "\r", frame art, label, and trailing padding.
    std::vector<std::string> frames_;

    // Index of the next frame to display.
    std::size_t current_frame_ = 0;

    // Length of the longest pre-formatted frame string (used by Clear()).
    std::size_t max_line_length_ = 0;

    // Loads frames from a configuration file, skipping comments and blank lines.
    // @return Non-empty vector on success; empty vector if file is missing or has no valid frames.
    static std::vector<std::string> LoadFramesFromFile(std::string_view path);

    // Returns the hardcoded fallback frame set: | / - backslash.
    static std::vector<std::string> DefaultFrames();

    // Wraps raw frame strings into pre-formatted output lines with CR, label, and padding.
    void BuildFormattedLines(const std::vector<std::string> &raw_frames);
};


#endif // TERMINAL_SPINNER_H