#ifndef CSV_CONVERTER_H
#define CSV_CONVERTER_H

#include <cstdint>
#include <functional>
#include <string>


using CsvProgressCallback = std::function<void(uint64_t frames_processed,
                                               uint64_t bytes_processed,
                                               uint64_t total_bytes)>;


struct CsvConversionResult {
    bool success = false;
    uint64_t frames_converted = 0;
    uint64_t points_converted = 0;
    uint64_t bytes_read = 0;
    std::string error_message;
};


// Binary-to-CSV converter for the LIDARPCD file format.
//
// Reads the binary file written by BufferedBinaryWriter and produces a CSV
// with columns: frame_index, timestamp_ns, point_index, x, y, z, intensity
class BinaryToCsvConverter {
public:
    BinaryToCsvConverter() = default;

    void set_progress_callback(CsvProgressCallback callback);

    void set_progress_interval(uint64_t interval);

    CsvConversionResult convert(const std::string &binary_path,
                                const std::string &csv_path) const;

    static std::string default_csv_path(const std::string &binary_path);

private:
    CsvProgressCallback progress_callback_;
    uint64_t progress_interval_ = 100;
};


#endif // CSV_CONVERTER_H