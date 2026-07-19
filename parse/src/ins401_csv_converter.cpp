#include "ins401_csv_converter.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include <spdlog/spdlog.h>


namespace {
	constexpr std::string_view kModule = "INS401Converter";
	constexpr std::size_t kBatchSize = 1024;
	constexpr std::size_t kWriteBufferSize = 256 * 1024;

	// Packet lengths (must match ins401_protocol.h)
	constexpr std::size_t kGnssSolutionLen = 77;
	constexpr std::size_t kInsSolutionLen = 110;
	constexpr std::size_t kRawImuLen = 30;
	constexpr std::size_t kDiagnosticLen = 22;

	// Self-contained data structures (mirrors of ins401_data_type.h)
	struct GNSSSolutionData {
		std::uint16_t gps_week;
		std::uint32_t gps_millisecs;
		std::uint8_t position_type;
		double latitude;
		double longitude;
		double height;
		float latitude_std;
		float longitude_std;
		float height_std;
		std::uint8_t num_of_SVs;
		std::uint8_t num_of_SVs_in_solution;
		float hdop;
		float diffage;
		float north_vel;
		float east_vel;
		float up_vel;
		float north_vel_std;
		float east_vel_std;
		float up_vel_std;
	};

	struct INSSolutionData {
		std::uint16_t gps_week;
		std::uint32_t gps_millisecs;
		std::uint8_t ins_status;
		std::uint8_t ins_position_type;
		double latitude;
		double longitude;
		double height;
		float north_vel;
		float east_vel;
		float up_vel;
		float longitudinal_vel;
		float lateral_vel;
		float roll;
		float pitch;
		float heading;
		float latitude_std;
		float longitude_std;
		float height_std;
		float north_vel_std;
		float east_vel_std;
		float up_vel_std;
		float long_vel_std;
		float lat_vel_std;
		float roll_std;
		float pitch_std;
		float heading_std;
		std::uint16_t continent_id;
	};

	struct DiagnosticMessage {
		std::uint16_t gps_week;
		std::uint32_t gps_millisecs;
		std::array<int, 32> device_status;
		float imu_temperature;
		float mcu_temperature;
		float gnss_chip_temperature;
	};

	struct RawIMUData {
		std::uint16_t gps_week;
		std::uint32_t gps_millisecs;
		float acc_x;
		float acc_y;
		float acc_z;
		float gyro_x;
		float gyro_y;
		float gyro_z;
	};


	GNSSSolutionData ParseGNSSSolution(const std::uint8_t *p) {
		GNSSSolutionData d{};
		std::memcpy(&d.gps_week, p, sizeof(std::uint16_t));
		std::memcpy(&d.gps_millisecs, p + 2, sizeof(std::uint32_t));
		d.position_type = p[6];
		std::memcpy(&d.latitude, p + 7, sizeof(double));
		std::memcpy(&d.longitude, p + 15, sizeof(double));
		std::memcpy(&d.height, p + 23, sizeof(double));
		std::memcpy(&d.latitude_std, p + 31, sizeof(float));
		std::memcpy(&d.longitude_std, p + 35, sizeof(float));
		std::memcpy(&d.height_std, p + 39, sizeof(float));
		d.num_of_SVs = p[43];
		d.num_of_SVs_in_solution = p[44];
		std::memcpy(&d.hdop, p + 45, sizeof(float));
		std::memcpy(&d.diffage, p + 49, sizeof(float));
		std::memcpy(&d.north_vel, p + 53, sizeof(float));
		std::memcpy(&d.east_vel, p + 57, sizeof(float));
		std::memcpy(&d.up_vel, p + 61, sizeof(float));
		std::memcpy(&d.north_vel_std, p + 65, sizeof(float));
		std::memcpy(&d.east_vel_std, p + 69, sizeof(float));
		std::memcpy(&d.up_vel_std, p + 73, sizeof(float));
		return d;
	}

	INSSolutionData ParseINSSolution(const std::uint8_t *p) {
		INSSolutionData d{};
		std::memcpy(&d.gps_week, p, sizeof(std::uint16_t));
		std::memcpy(&d.gps_millisecs, p + 2, sizeof(std::uint32_t));
		d.ins_status = p[6];
		d.ins_position_type = p[7];
		std::memcpy(&d.latitude, p + 8, sizeof(double));
		std::memcpy(&d.longitude, p + 16, sizeof(double));
		std::memcpy(&d.height, p + 24, sizeof(double));
		std::memcpy(&d.north_vel, p + 32, sizeof(float));
		std::memcpy(&d.east_vel, p + 36, sizeof(float));
		std::memcpy(&d.up_vel, p + 40, sizeof(float));
		std::memcpy(&d.longitudinal_vel, p + 44, sizeof(float));
		std::memcpy(&d.lateral_vel, p + 48, sizeof(float));
		std::memcpy(&d.roll, p + 52, sizeof(float));
		std::memcpy(&d.pitch, p + 56, sizeof(float));
		std::memcpy(&d.heading, p + 60, sizeof(float));
		std::memcpy(&d.latitude_std, p + 64, sizeof(float));
		std::memcpy(&d.longitude_std, p + 68, sizeof(float));
		std::memcpy(&d.height_std, p + 72, sizeof(float));
		std::memcpy(&d.north_vel_std, p + 76, sizeof(float));
		std::memcpy(&d.east_vel_std, p + 80, sizeof(float));
		std::memcpy(&d.up_vel_std, p + 84, sizeof(float));
		std::memcpy(&d.long_vel_std, p + 88, sizeof(float));
		std::memcpy(&d.lat_vel_std, p + 92, sizeof(float));
		std::memcpy(&d.roll_std, p + 96, sizeof(float));
		std::memcpy(&d.pitch_std, p + 100, sizeof(float));
		std::memcpy(&d.heading_std, p + 104, sizeof(float));
		std::memcpy(&d.continent_id, p + 108, sizeof(std::uint16_t));
		return d;
	}

	DiagnosticMessage ParseDiagnostic(const std::uint8_t *p) {
		DiagnosticMessage d{};
		std::memcpy(&d.gps_week, p, sizeof(std::uint16_t));
		std::memcpy(&d.gps_millisecs, p + 2, sizeof(std::uint32_t));
		std::uint32_t status_value;
		std::memcpy(&status_value, p + 6, sizeof(std::uint32_t));
		const std::bitset<32> bs(status_value);
		for (int i = 0; i < 32; ++i) {
			d.device_status[i] = bs[i];
		}
		std::memcpy(&d.imu_temperature, p + 10, sizeof(float));
		std::memcpy(&d.mcu_temperature, p + 14, sizeof(float));
		std::memcpy(&d.gnss_chip_temperature, p + 18, sizeof(float));
		return d;
	}

	RawIMUData ParseRawIMU(const std::uint8_t *p) {
		RawIMUData d{};
		std::memcpy(&d.gps_week, p, sizeof(std::uint16_t));
		std::memcpy(&d.gps_millisecs, p + 2, sizeof(std::uint32_t));
		std::memcpy(&d.acc_x, p + 6, sizeof(float));
		std::memcpy(&d.acc_y, p + 10, sizeof(float));
		std::memcpy(&d.acc_z, p + 14, sizeof(float));
		std::memcpy(&d.gyro_x, p + 18, sizeof(float));
		std::memcpy(&d.gyro_y, p + 22, sizeof(float));
		std::memcpy(&d.gyro_z, p + 26, sizeof(float));
		return d;
	}
}  // namespace


namespace CsvConverter {

	std::size_t ConvertGnssBin(const std::string &bin_path, const std::string &csv_path) {
		std::ifstream bin_in(bin_path, std::ios::in | std::ios::binary);
		if (!bin_in.is_open()) {
			spdlog::error("[{}] Cannot open GNSS binary file: {}", kModule, bin_path);
			return 0;
		}

		std::ofstream csv_out(csv_path, std::ios::out);
		if (!csv_out.is_open()) {
			spdlog::error("[{}] Cannot create GNSS CSV file: {}", kModule, csv_path);
			return 0;
		}

		std::vector<char> csv_buffer(kWriteBufferSize);
		csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
		csv_out << "GPS_Week,GPS_MS[ms],Position_Type,Latitude[deg],Longitude[deg],Height[m],"
				   "Latitude_STD[m],Longitude_STD[m],Height_STD[m],Num_of_SVs,Num_of_SVs_in_Solution,"
				   "Hdop,Diffage[s],North_Vel[m/s],East_Vel[m/s],Up_Vel[m/s],"
				   "North_Vel_STD[m/s],East_Vel_STD[m/s],Up_Vel_STD[m/s]\n";

		std::vector<std::uint8_t> record(kGnssSolutionLen);
		fmt::memory_buffer fmt_buf;
		std::size_t count = 0;

		while (bin_in.read(reinterpret_cast<char *>(record.data()), static_cast<std::streamsize>(kGnssSolutionLen))) {
			const auto gnss = ParseGNSSSolution(record.data());
			fmt::format_to(std::back_inserter(fmt_buf), "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
						   gnss.gps_week, gnss.gps_millisecs, gnss.position_type,
						   gnss.latitude, gnss.longitude, gnss.height,
						   gnss.latitude_std, gnss.longitude_std, gnss.height_std,
						   gnss.num_of_SVs, gnss.num_of_SVs_in_solution,
						   gnss.hdop, gnss.diffage,
						   gnss.north_vel, gnss.east_vel, gnss.up_vel,
						   gnss.north_vel_std, gnss.east_vel_std, gnss.up_vel_std);
			if (++count % kBatchSize == 0) {
				csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
				fmt_buf.clear();
			}
		}

		if (fmt_buf.size() > 0) {
			csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
		}

		spdlog::info("[{}] GNSS: {} records converted: {} -> {}", kModule, count, bin_path, csv_path);
		return count;
	}


	std::size_t ConvertInsBin(const std::string &bin_path, const std::string &csv_path) {
		std::ifstream bin_in(bin_path, std::ios::in | std::ios::binary);
		if (!bin_in.is_open()) {
			spdlog::error("[{}] Cannot open INS binary file: {}", kModule, bin_path);
			return 0;
		}

		std::ofstream csv_out(csv_path, std::ios::out);
		if (!csv_out.is_open()) {
			spdlog::error("[{}] Cannot create INS CSV file: {}", kModule, csv_path);
			return 0;
		}

		std::vector<char> csv_buffer(kWriteBufferSize);
		csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
		csv_out << "GPS_Week,GPS_MS[ms],INS_Status,INS_Position_Type,Latitude[deg],Longitude[deg],Height[m],"
				   "North_Vel[m/s],East_Vel[m/s],Up_Vel[m/s],Longitudinal_Vel[m/s],Lateral_Vel[m/s],"
				   "Roll[deg],Pitch[deg],Heading[deg],"
				   "Latitude_STD[m],Longitude_STD[m],Height_STD[m],"
				   "North_Vel_STD[m/s],East_Vel_STD[m/s],Up_Vel_STD[m/s],"
				   "Longitudinal_Vel_STD[m/s],Lateral_Vel_STD[m/s],"
				   "Roll_STD[deg],Pitch_STD[deg],Heading_STD[deg],Continent_ID\n";

		std::vector<std::uint8_t> record(kInsSolutionLen);
		fmt::memory_buffer fmt_buf;
		std::size_t count = 0;

		while (bin_in.read(reinterpret_cast<char *>(record.data()), static_cast<std::streamsize>(kInsSolutionLen))) {
			const auto ins = ParseINSSolution(record.data());
			fmt::format_to(
					std::back_inserter(fmt_buf), "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
					ins.gps_week, ins.gps_millisecs, ins.ins_status, ins.ins_position_type,
					ins.latitude, ins.longitude, ins.height,
					ins.north_vel, ins.east_vel, ins.up_vel, ins.longitudinal_vel, ins.lateral_vel,
					ins.roll, ins.pitch, ins.heading,
					ins.latitude_std, ins.longitude_std, ins.height_std,
					ins.north_vel_std, ins.east_vel_std, ins.up_vel_std,
					ins.long_vel_std, ins.lat_vel_std,
					ins.roll_std, ins.pitch_std, ins.heading_std, ins.continent_id);
			if (++count % kBatchSize == 0) {
				csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
				fmt_buf.clear();
			}
		}

		if (fmt_buf.size() > 0) {
			csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
		}

		spdlog::info("[{}] INS: {} records converted: {} -> {}", kModule, count, bin_path, csv_path);
		return count;
	}


	std::size_t ConvertImuBin(const std::string &bin_path, const std::string &csv_path) {
		std::ifstream bin_in(bin_path, std::ios::in | std::ios::binary);
		if (!bin_in.is_open()) {
			spdlog::error("[{}] Cannot open IMU binary file: {}", kModule, bin_path);
			return 0;
		}

		std::ofstream csv_out(csv_path, std::ios::out);
		if (!csv_out.is_open()) {
			spdlog::error("[{}] Cannot create IMU CSV file: {}", kModule, csv_path);
			return 0;
		}

		std::vector<char> csv_buffer(kWriteBufferSize);
		csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
		csv_out << "GPS_Week,GPS_MS[ms],Acc_X[m/s^2],Acc_Y[m/s^2],Acc_Z[m/s^2],"
				   "Gyro_X[deg/s],Gyro_Y[deg/s],Gyro_Z[deg/s]\n";

		std::vector<std::uint8_t> record(kRawImuLen);
		fmt::memory_buffer fmt_buf;
		std::size_t count = 0;

		while (bin_in.read(reinterpret_cast<char *>(record.data()), static_cast<std::streamsize>(kRawImuLen))) {
			const auto imu = ParseRawIMU(record.data());
			fmt::format_to(std::back_inserter(fmt_buf), "{},{},{},{},{},{},{},{}\n",
						   imu.gps_week, imu.gps_millisecs,
						   imu.acc_x, imu.acc_y, imu.acc_z,
						   imu.gyro_x, imu.gyro_y, imu.gyro_z);
			if (++count % kBatchSize == 0) {
				csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
				fmt_buf.clear();
			}
		}

		if (fmt_buf.size() > 0) {
			csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
		}

		spdlog::info("[{}] IMU: {} records converted: {} -> {}", kModule, count, bin_path, csv_path);
		return count;
	}


	std::size_t ConvertDiagnosticBin(const std::string &bin_path, const std::string &csv_path) {
		std::ifstream bin_in(bin_path, std::ios::in | std::ios::binary);
		if (!bin_in.is_open()) {
			spdlog::error("[{}] Cannot open diagnostic binary file: {}", kModule, bin_path);
			return 0;
		}

		std::ofstream csv_out(csv_path, std::ios::out);
		if (!csv_out.is_open()) {
			spdlog::error("[{}] Cannot create diagnostic CSV file: {}", kModule, csv_path);
			return 0;
		}

		std::vector<char> csv_buffer(kWriteBufferSize);
		csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));
		csv_out << "GPS_Week,GPS_MS[ms],Device_Status,IMU_Temperature[C],MCU_Temperature[C],GNSS_Chip_Temperature[C]\n";

		std::vector<std::uint8_t> record(kDiagnosticLen);
		fmt::memory_buffer fmt_buf;
		std::size_t count = 0;

		while (bin_in.read(reinterpret_cast<char *>(record.data()), static_cast<std::streamsize>(kDiagnosticLen))) {
			const auto diag = ParseDiagnostic(record.data());
			fmt::format_to(std::back_inserter(fmt_buf), "{},{},\"[", diag.gps_week, diag.gps_millisecs);
			for (int i = 0; i < 32; ++i) {
				if (i > 0) fmt::format_to(std::back_inserter(fmt_buf), ",");
				fmt::format_to(std::back_inserter(fmt_buf), "{}", diag.device_status[i]);
			}
			fmt::format_to(std::back_inserter(fmt_buf), "]\",{},{},{}\n",
						   diag.imu_temperature, diag.mcu_temperature, diag.gnss_chip_temperature);
			if (++count % kBatchSize == 0) {
				csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
				fmt_buf.clear();
			}
		}

		if (fmt_buf.size() > 0) {
			csv_out.write(fmt_buf.data(), static_cast<std::streamsize>(fmt_buf.size()));
		}

		spdlog::info("[{}] Diagnostic: {} records converted: {} -> {}", kModule, count, bin_path, csv_path);
		return count;
	}

}  // namespace CsvConverter
