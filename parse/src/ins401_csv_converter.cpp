#include "ins401_csv_converter.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "ins401_wire_format.h"


namespace {
	constexpr std::string_view kModule = "INS401Converter";
	constexpr std::size_t kBatchSize = 1024;
	constexpr std::size_t kWriteBufferSize = 256 * 1024;

	// Shared conversion skeleton: fixed-size records -> CSV rows, batched
	// through a fmt buffer. Offline tool: failures log and return 0 so the
	// remaining files still convert.
	template<typename RowFormatter>
	std::size_t ConvertFixedRecordBin(const std::string &bin_path, const std::string &csv_path, std::string_view type,
									  std::size_t record_len, const char *csv_header, RowFormatter format_row) {
		std::ifstream bin_in(bin_path, std::ios::in | std::ios::binary);
		if (!bin_in.is_open()) {
			spdlog::error("[{}] Cannot open {} binary file: {}", kModule, type, bin_path);
			return 0;
		}

		std::ofstream csv_out(csv_path, std::ios::out);
		if (!csv_out.is_open()) {
			spdlog::error("[{}] Cannot create {} CSV file: {}", kModule, type, csv_path);
			return 0;
		}

		std::vector<char> csv_buffer(kWriteBufferSize);
		csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
		csv_out << csv_header;

		std::vector<std::uint8_t> record(record_len);
		fmt::memory_buffer fmt_buf;
		std::size_t count = 0;

		while (bin_in.read(reinterpret_cast<char *>(record.data()), static_cast<std::streamsize>(record_len))) {
			format_row(fmt_buf, record.data());
			if (++count % kBatchSize == 0) {
				csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
				fmt_buf.clear();
			}
		}

		if (fmt_buf.size() > 0) {
			csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
		}

		spdlog::info("[{}] {}: {} records converted: {} -> {}", kModule, type, count, bin_path, csv_path);
		return count;
	}
}  // namespace


namespace CsvConverter {

	std::size_t ConvertGnssBin(const std::string &bin_path, const std::string &csv_path) {
		return ConvertFixedRecordBin(
				bin_path, csv_path, "GNSS", INS401::GNSS_SOLUTION_PACKET_LENGTH,
				"GPS_Week,GPS_MS[ms],Position_Type,Latitude[deg],Longitude[deg],Height[m],"
				"Latitude_STD[m],Longitude_STD[m],Height_STD[m],Num_of_SVs,Num_of_SVs_in_Solution,"
				"Hdop,Diffage[s],North_Vel[m/s],East_Vel[m/s],Up_Vel[m/s],"
				"North_Vel_STD[m/s],East_Vel_STD[m/s],Up_Vel_STD[m/s]\n",
				[](fmt::memory_buffer &buf, const std::uint8_t *rec) {
					const auto gnss = INS401::ParseGNSSSolutionPayload(rec);
					fmt::format_to(std::back_inserter(buf), "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
								   gnss.gps_week, gnss.gps_millisecs, gnss.position_type,
								   gnss.latitude, gnss.longitude, gnss.height,
								   gnss.latitude_std, gnss.longitude_std, gnss.height_std,
								   gnss.num_of_SVs, gnss.num_of_SVs_in_solution,
								   gnss.hdop, gnss.diffage,
								   gnss.north_vel, gnss.east_vel, gnss.up_vel,
								   gnss.north_vel_std, gnss.east_vel_std, gnss.up_vel_std);
				});
	}


	std::size_t ConvertInsBin(const std::string &bin_path, const std::string &csv_path) {
		return ConvertFixedRecordBin(
				bin_path, csv_path, "INS", INS401::INS_SOLUTION_PACKET_LENGTH,
				"GPS_Week,GPS_MS[ms],INS_Status,INS_Position_Type,Latitude[deg],Longitude[deg],Height[m],"
				"North_Vel[m/s],East_Vel[m/s],Up_Vel[m/s],Longitudinal_Vel[m/s],Lateral_Vel[m/s],"
				"Roll[deg],Pitch[deg],Heading[deg],"
				"Latitude_STD[m],Longitude_STD[m],Height_STD[m],"
				"North_Vel_STD[m/s],East_Vel_STD[m/s],Up_Vel_STD[m/s],"
				"Longitudinal_Vel_STD[m/s],Lateral_Vel_STD[m/s],"
				"Roll_STD[deg],Pitch_STD[deg],Heading_STD[deg],Continent_ID\n",
				[](fmt::memory_buffer &buf, const std::uint8_t *rec) {
					const auto ins = INS401::ParseINSSolutionPayload(rec);
					fmt::format_to(
							std::back_inserter(buf),
							"{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
							ins.gps_week, ins.gps_millisecs, ins.ins_status, ins.ins_position_type,
							ins.latitude, ins.longitude, ins.height,
							ins.north_vel, ins.east_vel, ins.up_vel, ins.longitudinal_vel, ins.lateral_vel,
							ins.roll, ins.pitch, ins.heading,
							ins.latitude_std, ins.longitude_std, ins.height_std,
							ins.north_vel_std, ins.east_vel_std, ins.up_vel_std,
							ins.long_vel_std, ins.lat_vel_std,
							ins.roll_std, ins.pitch_std, ins.heading_std, ins.continent_id);
				});
	}


	std::size_t ConvertImuBin(const std::string &bin_path, const std::string &csv_path) {
		return ConvertFixedRecordBin(
				bin_path, csv_path, "IMU", INS401::RAW_IMU_DATA_LENGTH,
				"GPS_Week,GPS_MS[ms],Acc_X[m/s^2],Acc_Y[m/s^2],Acc_Z[m/s^2],"
				"Gyro_X[deg/s],Gyro_Y[deg/s],Gyro_Z[deg/s]\n",
				[](fmt::memory_buffer &buf, const std::uint8_t *rec) {
					const auto imu = INS401::ParseRawIMUPayload(rec);
					fmt::format_to(std::back_inserter(buf), "{},{},{},{},{},{},{},{}\n",
								   imu.gps_week, imu.gps_millisecs,
								   imu.acc_x, imu.acc_y, imu.acc_z,
								   imu.gyro_x, imu.gyro_y, imu.gyro_z);
				});
	}


	std::size_t ConvertDiagnosticBin(const std::string &bin_path, const std::string &csv_path) {
		return ConvertFixedRecordBin(
				bin_path, csv_path, "Diagnostic", INS401::DIAGNOSTIC_MESSAGE_LENGTH,
				"GPS_Week,GPS_MS[ms],Device_Status,IMU_Temperature[C],MCU_Temperature[C],GNSS_Chip_Temperature[C]\n",
				[](fmt::memory_buffer &buf, const std::uint8_t *rec) {
					const auto diag = INS401::ParseDiagnosticPayload(rec);
					fmt::format_to(std::back_inserter(buf), "{},{},\"[", diag.gps_week, diag.gps_millisecs);
					for (int i = 0; i < 32; ++i) {
						if (i > 0) fmt::format_to(std::back_inserter(buf), ",");
						fmt::format_to(std::back_inserter(buf), "{}", diag.device_status[i]);
					}
					fmt::format_to(std::back_inserter(buf), "]\",{},{},{}\n",
								   diag.imu_temperature, diag.mcu_temperature, diag.gnss_chip_temperature);
				});
	}

}  // namespace CsvConverter
