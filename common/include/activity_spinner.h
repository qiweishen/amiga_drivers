/// @file common/activity_spinner.h
/// @brief Activity-driven terminal spinner shared across all drivers
///
/// Unlike the original TerminalSpinner which ticks unconditionally, this spinner
/// only animates when recent activity has been reported (within idle_threshold_s)
/// Thread-safe: any thread may call ReportActivity(), while a single owner thread
/// calls Tick(). Stdout writes are mutex-protected to avoid interleaving with logs

#ifndef COMMON_ACTIVITY_SPINNER_H
#define COMMON_ACTIVITY_SPINNER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>


namespace Common {

class ActivitySpinner {
public:
	// Construct spinner with frame file and idle threshold in seconds
	// If file cannot be read or is empty, falls back to a built-in default set
	explicit ActivitySpinner(std::string_view frames_file_path, double idle_threshold_s = 1.25);

	~ActivitySpinner();

	// Non-copyable, non-movable: owns exclusive cursor-line state on stdout
	ActivitySpinner(const ActivitySpinner &) = delete;
	ActivitySpinner &operator=(const ActivitySpinner &) = delete;
	ActivitySpinner(ActivitySpinner &&) = delete;
	ActivitySpinner &operator=(ActivitySpinner &&) = delete;

	// Signal that data activity occurred, thread-safe and lock-free
	// Call from any data-receiving thread (e.g., receiver callbacks)
	void ReportActivity();

	// Advance frame and render if active, clear line if idle
	// Called from the owner thread's polling loop (~5 Hz)
	void Tick();

	// Clear the spinner line from terminal (restore clean state)
	void Clear();

	// Start rendering (default state after construction)
	void Attach();

	// Stop rendering and clear the line
	void Detach();

private:
	// Render the current frame to stdout under mutex protection
	void RenderFrame();

	// Overwrite the spinner line with spaces
	void ClearLine();

	// Current steady_clock time in milliseconds
	static int64_t NowMs();

	// Load frames from a configuration file, skipping comments and blank lines
	static std::vector<std::string> LoadFramesFromFile(std::string_view path);

	// Hardcoded fallback frame set
	static std::vector<std::string> DefaultFrames();

	// Wrap raw frame strings into pre-formatted output lines with CR, label, and padding
	void BuildFormattedLines(const std::vector<std::string> &raw_frames);

	// Pre-formatted frame strings ready for direct fwrite to stdout
	std::vector<std::string> frames_;

	// Index of the next frame to display
	std::size_t current_frame_ = 0;

	// Length of the longest pre-formatted frame string (used by ClearLine)
	std::size_t max_line_length_ = 0;

	// Seconds of inactivity before spinner goes idle
	double idle_threshold_s_;

	// Protects stdout writes from concurrent Tick/Clear/log interleaving
	std::mutex stdout_mutex_;

	// Whether the spinner is attached and should render
	std::atomic<bool> spinning_{false};

	// Last activity timestamp in steady_clock milliseconds (lock-free update)
	std::atomic<int64_t> last_activity_ms_{0};
};

} // namespace Common


#endif // COMMON_ACTIVITY_SPINNER_H
