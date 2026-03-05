/// @file activity_spinner.cpp
/// @brief Implementation of Common::ActivitySpinner -- activity-driven terminal spinner
///        All output uses cstdio (fwrite/fflush) to bypass spdlog sinks entirely

#include "activity_spinner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "utility.h"


namespace {
	// Label displayed alongside every spinner frame
	constexpr std::string_view kLabel = "System Runing o_0";

	// Minimum trailing-space padding so shorter frames overwrite remnants of longer ones
	constexpr std::size_t kMinPadWidth = 4;

	// Count UTF-8 code points in a string (one display column per code point)
	// Correct for ASCII, Latin, Braille, and most symbols used in spinner frames
	std::size_t Utf8CodePointCount(std::string_view s) {
		std::size_t count = 0;
		for (unsigned char c : s) {
			// Count only leading bytes: 0xxxxxxx or 11xxxxxx
			if ((c & 0xC0) != 0x80) {
				++count;
			}
		}
		return count;
	}

	// Global pointer for the pre-log callback to reach the active spinner
	std::atomic<Common::ActivitySpinner *> g_active_spinner{nullptr};

	void SpinnerPreLogHook() {
		if (auto *sp = g_active_spinner.load(std::memory_order_acquire)) {
			sp->ReportActivity();
		}
	}
}  // namespace


namespace Common {

ActivitySpinner::ActivitySpinner(std::string_view frames_file_path, double idle_threshold_s)
	: idle_threshold_s_(idle_threshold_s) {
	std::vector<std::string> raw_frames = LoadFramesFromFile(frames_file_path);
	if (raw_frames.empty()) {
		raw_frames = DefaultFrames();
	}
	BuildFormattedLines(raw_frames);
	last_activity_ms_.store(NowMs(), std::memory_order_release);
}


ActivitySpinner::~ActivitySpinner() {
	Detach();
	Clear();
}


void ActivitySpinner::ReportActivity() {
	auto now = NowMs();
	// Fast path: not spinning, just update timestamp
	if (!spinning_.load(std::memory_order_acquire)) {
		last_activity_ms_.store(now, std::memory_order_release);
		return;
	}
	// Slow path: clear spinner line before log output
	std::lock_guard<std::mutex> lock(stdout_mutex_);
	last_activity_ms_.store(now, std::memory_order_release);
	if (spinning_.load(std::memory_order_relaxed)) {
		ClearLine();
		spinning_.store(false, std::memory_order_release);
	}
}


void ActivitySpinner::Tick() {
	std::lock_guard<std::mutex> lock(stdout_mutex_);
	int64_t now = NowMs();
	int64_t last = last_activity_ms_.load(std::memory_order_relaxed);
	double idle_s = static_cast<double>(now - last) / 1000.0;
	if (idle_s >= idle_threshold_s_) {
		spinning_.store(true, std::memory_order_release);
		RenderFrame();
	}
}


void ActivitySpinner::Clear() {
	std::lock_guard<std::mutex> lock(stdout_mutex_);
	if (spinning_.load(std::memory_order_relaxed)) {
		ClearLine();
		spinning_.store(false, std::memory_order_release);
	}
}


void ActivitySpinner::Attach() {
	g_active_spinner.store(this, std::memory_order_release);
	Common::Log::set_pre_log_callback(SpinnerPreLogHook);
	last_activity_ms_.store(NowMs(), std::memory_order_release);
}


void ActivitySpinner::Detach() {
	Common::Log::set_pre_log_callback(nullptr);
	g_active_spinner.store(nullptr, std::memory_order_release);
}


void ActivitySpinner::RenderFrame() {
	const std::string &line = frames_[current_frame_];
	std::fwrite(line.data(), 1, line.size(), stdout);
	std::fflush(stdout);
	current_frame_ = (current_frame_ + 1) % frames_.size();
}


void ActivitySpinner::ClearLine() {
	// Overwrite the spinner line with spaces, then return cursor to column 0
	std::string blank(max_line_length_ + 1, ' ');
	blank[0] = '\r';
	blank.push_back('\r');
	std::fwrite(blank.data(), 1, blank.size(), stdout);
	std::fflush(stdout);
}


int64_t ActivitySpinner::NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count();
}


std::vector<std::string> ActivitySpinner::LoadFramesFromFile(std::string_view path) {
	std::string path_str{path};
	std::ifstream file{path_str};
	if (!file.is_open()) {
		std::fprintf(stderr, "[ActivitySpinner] Could not open frames file: %.*s, using defaults\n",
					 static_cast<int>(path.size()), path.data());
		return {};
	}

	std::vector<std::string> frames;
	std::string line;
	while (std::getline(file, line)) {
		// Strip trailing whitespace (handles \r\n on Linux if file was edited on Windows)
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		// Skip empty lines and comment lines
		if (line.empty() || line.front() == '#') {
			continue;
		}
		frames.push_back(std::move(line));
		line = {};	// Moved-from string, reinitialize for next iteration
	}
	return frames;
}


std::vector<std::string> ActivitySpinner::DefaultFrames() {
	return {"|", "/", "-", "\\"};
}


void ActivitySpinner::BuildFormattedLines(const std::vector<std::string> &raw_frames) {
	// Determine the widest raw frame in display columns (code points, not bytes)
	std::size_t max_frame_display_width = 0;
	for (const auto &f : raw_frames) {
		max_frame_display_width = std::max(max_frame_display_width, Utf8CodePointCount(f));
	}

	// Visible portion: "  <frame><pad>  <label>  "
	const std::size_t visible_width = 2 + max_frame_display_width + 2 + kLabel.size() + kMinPadWidth;

	frames_.clear();
	frames_.reserve(raw_frames.size());

	for (const auto &raw : raw_frames) {
		std::string formatted;
		formatted.reserve(1 + raw.size() + max_frame_display_width + 2 + kLabel.size() + kMinPadWidth + 4);

		formatted.push_back('\r');
		formatted.append("  ");
		formatted.append(raw);
		// Pad frame column to uniform display width using code-point count
		const std::size_t raw_display_width = Utf8CodePointCount(raw);
		if (raw_display_width < max_frame_display_width) {
			formatted.append(max_frame_display_width - raw_display_width, ' ');
		}
		formatted.append("  ");
		formatted.append(kLabel);
		// Trailing spaces to overwrite stale characters from a previous longer line
		formatted.append(kMinPadWidth, ' ');

		frames_.push_back(std::move(formatted));
	}

	max_line_length_ = visible_width;
}

}  // namespace Common
