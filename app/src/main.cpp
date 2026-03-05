#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "ins401_csv_converter.h"
#include "lms4xxx_csv_converter.h"


namespace fs = std::filesystem;


namespace {

	struct ConvertResult {
		std::string bin_path;
		std::string csv_path;
		std::string type;
		std::size_t records = 0;
	};


	ConvertResult ConvertFile(const fs::path &bin_file, const fs::path &csv_dir) {
		const std::string filename = bin_file.filename().string();
		const std::string stem = bin_file.stem().string();
		const std::string csv_name = stem + ".csv";
		const std::string csv_path = (csv_dir / csv_name).string();
		const std::string bin_path = bin_file.string();

		ConvertResult result;
		result.bin_path = bin_path;
		result.csv_path = csv_path;

		if (filename.rfind("scan_", 0) == 0) {
			result.type = "LMS4xxx scan";
			result.records = CsvConverter::ConvertLms4xxxBin(bin_path, csv_path);
		} else if (filename.rfind("gnss_", 0) == 0) {
			result.type = "INS401 GNSS";
			result.records = CsvConverter::ConvertGnssBin(bin_path, csv_path);
		} else if (filename.rfind("ins_", 0) == 0) {
			result.type = "INS401 INS";
			result.records = CsvConverter::ConvertInsBin(bin_path, csv_path);
		} else if (filename.rfind("imu_", 0) == 0) {
			result.type = "INS401 IMU";
			result.records = CsvConverter::ConvertImuBin(bin_path, csv_path);
		} else if (filename.rfind("diagnostic_", 0) == 0) {
			result.type = "INS401 Diagnostic";
			result.records = CsvConverter::ConvertDiagnosticBin(bin_path, csv_path);
		} else {
			spdlog::warn("Skipping unrecognized file: {}", bin_path);
			result.type = "unknown";
		}

		return result;
	}

}  // namespace


int main(int argc, char *argv[]) {
	if (argc < 2) {
		fmt::print(stderr,
				   "Usage: {} <path>\n"
				   "\n"
				   "  <path> can be:\n"
				   "    - A data directory (e.g., ./data/20260305_123456)\n"
				   "      Scans bin/ subdirectory for all .bin files\n"
				   "    - A single .bin file\n"
				   "\n"
				   "  File name prefixes determine conversion type:\n"
				   "    scan_*.bin       -> LMS4xxx scan data\n"
				   "    gnss_*.bin       -> INS401 GNSS solution\n"
				   "    ins_*.bin        -> INS401 INS solution\n"
				   "    imu_*.bin        -> INS401 raw IMU\n"
				   "    diagnostic_*.bin -> INS401 diagnostic\n",
				   argv[0]);
		return 1;
	}

	const fs::path input_path(argv[1]);

	if (!fs::exists(input_path)) {
		fmt::print(stderr, "Error: path does not exist: {}\n", input_path.string());
		return 1;
	}

	std::vector<ConvertResult> results;

	if (fs::is_directory(input_path)) {
		// Scan bin/ subdirectory
		const fs::path bin_dir = input_path / "bin";
		if (!fs::is_directory(bin_dir)) {
			fmt::print(stderr, "Error: no bin/ subdirectory found in: {}\n", input_path.string());
			return 1;
		}

		// CSV output goes to the parent data directory
		const fs::path csv_dir = input_path;

		// Collect and sort .bin files
		std::vector<fs::path> bin_files;
		for (const auto &entry : fs::directory_iterator(bin_dir)) {
			if (entry.is_regular_file() && entry.path().extension() == ".bin") {
				bin_files.push_back(entry.path());
			}
		}
		std::sort(bin_files.begin(), bin_files.end());

		if (bin_files.empty()) {
			fmt::print(stderr, "No .bin files found in: {}\n", bin_dir.string());
			return 1;
		}

		fmt::print("Found {} .bin file(s) in {}\n", bin_files.size(), bin_dir.string());

		for (const auto &bin_file : bin_files) {
			results.push_back(ConvertFile(bin_file, csv_dir));
		}
	} else if (fs::is_regular_file(input_path)) {
		// Single file mode
		if (input_path.extension() != ".bin") {
			fmt::print(stderr, "Error: expected a .bin file, got: {}\n", input_path.string());
			return 1;
		}

		// CSV output goes to the same directory as the .bin file's parent's parent
		// (i.e., if bin is at data/20xx/bin/scan_xxx.bin, CSV goes to data/20xx/)
		fs::path csv_dir = input_path.parent_path().parent_path();
		if (!fs::is_directory(csv_dir)) {
			csv_dir = input_path.parent_path();
		}

		results.push_back(ConvertFile(input_path, csv_dir));
	} else {
		fmt::print(stderr, "Error: path is not a file or directory: {}\n", input_path.string());
		return 1;
	}

	// Print summary
	fmt::print("\n=== Conversion Summary ===\n");
	std::size_t total_records = 0;
	std::size_t converted_files = 0;
	for (const auto &r : results) {
		if (r.type == "unknown") {
			continue;
		}
		fmt::print("  {:20s}  {:>10} records  {}\n", r.type, r.records, r.csv_path);
		total_records += r.records;
		++converted_files;
	}
	fmt::print("  {} file(s) converted, {} total records\n", converted_files, total_records);

	return 0;
}
