#ifndef LMS4XXX_CSV_CONVERTER_H
#define LMS4XXX_CSV_CONVERTER_H

#include <cstddef>
#include <string>


namespace CsvConverter {

	/// Convert a LMS4xxx binary scan recording to CSV.
	/// @return Number of scan frames converted.
	std::size_t ConvertLms4xxxBin(const std::string &bin_path, const std::string &csv_path);

}  // namespace CsvConverter

#endif  // LMS4XXX_CSV_CONVERTER_H
