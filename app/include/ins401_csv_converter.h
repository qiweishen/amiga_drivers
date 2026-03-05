#ifndef INS401_CSV_CONVERTER_H
#define INS401_CSV_CONVERTER_H

#include <cstddef>
#include <string>


namespace CsvConverter {

	/// Convert INS401 GNSS solution binary to CSV.
	std::size_t ConvertGnssBin(const std::string &bin_path, const std::string &csv_path);

	/// Convert INS401 INS solution binary to CSV.
	std::size_t ConvertInsBin(const std::string &bin_path, const std::string &csv_path);

	/// Convert INS401 raw IMU binary to CSV.
	std::size_t ConvertImuBin(const std::string &bin_path, const std::string &csv_path);

	/// Convert INS401 diagnostic binary to CSV.
	std::size_t ConvertDiagnosticBin(const std::string &bin_path, const std::string &csv_path);

}  // namespace CsvConverter

#endif  // INS401_CSV_CONVERTER_H
