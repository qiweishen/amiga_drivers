#include "lms4xxx_command_builder.h"

#include "lms4xxx_cola_b.h"
#include "lms4xxx_config.h"


namespace LMS4xxx {

	std::vector<std::uint8_t> CommandBuilder::BuildLogin() {
		// sMN SetAccessMode <user_level:Int8> <password:Uint32>
		std::vector<std::uint8_t> params;
		params.reserve(5);

		// User level: Authorized Client = 0x03
		params.push_back(kAuthorizedClientLevel);

		// Password hash: 0xF4724744 (Authorized Client)
		const auto pw = CoLaBCodec::EncodeUint32(kAuthorizedClientPassword);
		params.insert(params.end(), pw.begin(), pw.end());

		return CoLaBCodec::Encode(CommandType::kMethodByName, "SetAccessMode", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildRun() {
		// sMN Run (no parameters)
		return CoLaBCodec::Encode(CommandType::kMethodByName, "Run");
	}


	std::vector<std::uint8_t> CommandBuilder::BuildScanDataConfig(const ScanConfig &config) {
		// sWN LMDscandatacfg
		// Parameters (no spaces between them):
		//   Data channel: Uint8 Uint8  (01 00 = distance)
		//   Further channels: Uint8    (bitfield for remission/angle/quality)
		//   Reserved: Enum8            (01)
		//   Unit: Enum8                (00=RSSI digit, 01=REFL percent)
		//   Encoder: Uint8 Uint8       (00 00)
		//   Reserved: Bool1            (00)
		//   Device name: Bool1         (00/01)
		//   Reserved: Bool1            (00)
		//   Time: Bool1                (00/01)
		//   Output rate: Uint16        (00 01)

		std::vector<std::uint8_t> params;
		params.reserve(13);

		// Data channel (DIST1): 01 00 if enabled, 00 00 if not
		params.push_back(config.enable_distance ? 0x01 : 0x00);
		params.push_back(0x00);

		// Further channels: bitmask
		//   bit 0 = remission (RSSI/REFL)
		//   bit 1 = angle correction
		//   bit 2 = quality
		std::uint8_t further = 0x00;
		if (config.enable_rssi || config.enable_reflectance) {
			further |= 0x01;
		}
		if (config.enable_angle_correction) {
			further |= 0x02;
		}
		if (config.enable_quality) {
			further |= 0x04;
		}
		params.push_back(further);

		// Reserved: 01
		params.push_back(0x01);

		// Unit: 00=RSSI(digit), 01=REFL(percent)
		params.push_back(config.enable_reflectance ? 0x01 : 0x00);

		// Encoder: disabled (hardcoded)
		params.push_back(0x00);
		params.push_back(0x00);

		// Reserved: 00
		params.push_back(0x00);

		// Device name: disabled (hardcoded)
		params.push_back(0x00);

		// Reserved: 00
		params.push_back(0x00);

		// Time: 01 (always enable timestamp)
		params.push_back(0x01);

		// Output rate
		const auto rate = CoLaBCodec::EncodeUint16(config.output_rate);
		params.insert(params.end(), rate.begin(), rate.end());

		return CoLaBCodec::Encode(CommandType::kWriteByName, "LMDscandatacfg", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildOutputRange(const ScanConfig &config) {
		// sWN LMPoutputRange
		// Parameters:
		//   Reserved: Int16 = 00 01
		//   Angular resolution: Uint32 (1/10000 deg)
		//   Start angle: Int32 (1/10000 deg)
		//   Stop angle: Int32 (1/10000 deg)

		std::vector<std::uint8_t> params;
		params.reserve(14);

		// Reserved: 00 01
		const auto reserved = CoLaBCodec::EncodeUint16(0x0001);
		params.insert(params.end(), reserved.begin(), reserved.end());

		// Angular resolution (Uint32 in 1/10000 degrees)
		const auto resolution = CoLaBCodec::EncodeUint32(static_cast<std::uint32_t>(config.AngularResolutionDevice()));
		params.insert(params.end(), resolution.begin(), resolution.end());

		// Start angle (Int32 in 1/10000 degrees)
		const auto start = CoLaBCodec::EncodeInt32(config.StartAngleDevice());
		params.insert(params.end(), start.begin(), start.end());

		// Stop angle (Int32 in 1/10000 degrees)
		const auto stop = CoLaBCodec::EncodeInt32(config.StopAngleDevice());
		params.insert(params.end(), stop.begin(), stop.end());

		return CoLaBCodec::Encode(CommandType::kWriteByName, "LMPoutputRange", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildStartStream() {
		// sEN LMDscandata 01
		std::vector<std::uint8_t> params = { 0x01 };
		return CoLaBCodec::Encode(CommandType::kEventByName, "LMDscandata", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildStopStream() {
		// sEN LMDscandata 00
		std::vector<std::uint8_t> params = { 0x00 };
		return CoLaBCodec::Encode(CommandType::kEventByName, "LMDscandata", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildPollScan() {
		// sRN LMDscandata (no parameters)
		return CoLaBCodec::Encode(CommandType::kReadByName, "LMDscandata");
	}


	std::vector<std::uint8_t> CommandBuilder::BuildStartMeasurement() {
		// sMN LMCstartmeas (no parameters)
		return CoLaBCodec::Encode(CommandType::kMethodByName, "LMCstartmeas");
	}


	std::vector<std::uint8_t> CommandBuilder::BuildStopMeasurement() {
		// sMN LMCstopmeas (no parameters)
		return CoLaBCodec::Encode(CommandType::kMethodByName, "LMCstopmeas");
	}


	std::vector<std::uint8_t> CommandBuilder::BuildStandby() {
		// sMN LMCstandby (no parameters)
		return CoLaBCodec::Encode(CommandType::kMethodByName, "LMCstandby");
	}


	std::vector<std::uint8_t> CommandBuilder::BuildReboot() {
		// sMN mSCreboot (no parameters)
		return CoLaBCodec::Encode(CommandType::kMethodByName, "mSCreboot");
	}


	std::vector<std::uint8_t> CommandBuilder::BuildSaveParams() {
		// sMN mEEwriteall (no parameters)
		return CoLaBCodec::Encode(CommandType::kMethodByName, "mEEwriteall");
	}


	std::vector<std::uint8_t> CommandBuilder::BuildSetTimeSyncRole(std::uint8_t role) {
		// sWN TSCRole <status:Uint8>
		std::vector<std::uint8_t> params = { role };
		return CoLaBCodec::Encode(CommandType::kWriteByName, "TSCRole", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildSetDatetime(std::uint16_t year, std::uint8_t month, std::uint8_t day,
															   std::uint8_t hour, std::uint8_t minute, std::uint8_t second,
															   std::uint32_t microsecond) {
		// sMN LSPsetdatetime <year:Uint16> <month:Uint8> <day:Uint8>
		//                    <hour:Uint8> <minute:Uint8> <second:Uint8>
		//                    <microsecond:Uint32>
		std::vector<std::uint8_t> params;
		params.reserve(11);

		const auto y = CoLaBCodec::EncodeUint16(year);
		params.insert(params.end(), y.begin(), y.end());

		params.push_back(month);
		params.push_back(day);
		params.push_back(hour);
		params.push_back(minute);
		params.push_back(second);

		const auto us = CoLaBCodec::EncodeUint32(microsecond);
		params.insert(params.end(), us.begin(), us.end());

		return CoLaBCodec::Encode(CommandType::kMethodByName, "LSPsetdatetime", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildSetNtpServer(const std::array<std::uint8_t, 4> &ip_bytes) {
		// sWN TSCTCSrvAddr <ip1:Uint8> <ip2:Uint8> <ip3:Uint8> <ip4:Uint8>
		std::vector<std::uint8_t> params(ip_bytes.begin(), ip_bytes.end());
		return CoLaBCodec::Encode(CommandType::kWriteByName, "TSCTCSrvAddr", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildSetNtpUpdateTime(std::uint32_t seconds) {
		// sWN TSCTCupdatetime <seconds:Uint32>
		std::vector<std::uint8_t> params;
		params.reserve(4);
		const auto s = CoLaBCodec::EncodeUint32(seconds);
		params.insert(params.end(), s.begin(), s.end());
		return CoLaBCodec::Encode(CommandType::kWriteByName, "TSCTCupdatetime", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildSetNtpTimezone(std::uint8_t timezone) {
		// sWN TSCTCtimezone <timezone:Enum8>
		std::vector<std::uint8_t> params = { timezone };
		return CoLaBCodec::Encode(CommandType::kWriteByName, "TSCTCtimezone", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildMeanFilter(bool enable, std::uint16_t num_scans) {
		// sWN LFPmeanfilter <status:Bool1> <num_scans:Uint16> <reserved:Enum8=0>
		std::vector<std::uint8_t> params;
		params.reserve(4);
		params.push_back(enable ? 0x01 : 0x00);
		const auto n = CoLaBCodec::EncodeUint16(num_scans);
		params.insert(params.end(), n.begin(), n.end());
		params.push_back(0x00);	 // reserved
		return CoLaBCodec::Encode(CommandType::kWriteByName, "LFPmeanfilter", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildMedianFilter(bool enable) {
		// sWN LFPmedianfilter <status:Bool1> <reserved:Uint16=0003>
		std::vector<std::uint8_t> params;
		params.reserve(3);
		params.push_back(enable ? 0x01 : 0x00);
		const auto r = CoLaBCodec::EncodeUint16(0x0003);
		params.insert(params.end(), r.begin(), r.end());
		return CoLaBCodec::Encode(CommandType::kWriteByName, "LFPmedianfilter", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildFrontendEdgeFilter(bool enable) {
		// sWN LFPfrontendEdgefilter <status:Bool1> <reserved:Enum8=1>
		std::vector<std::uint8_t> params = { static_cast<std::uint8_t>(enable ? 0x01 : 0x00), 0x01 };
		return CoLaBCodec::Encode(CommandType::kWriteByName, "LFPfrontendEdgefilter", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildEdgeFilter(bool enable) {
		// sWN LFPedgefilter <status:Bool1>
		std::vector<std::uint8_t> params = { static_cast<std::uint8_t>(enable ? 0x01 : 0x00) };
		return CoLaBCodec::Encode(CommandType::kWriteByName, "LFPedgefilter", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildCubicAreaFilter(bool enable, std::uint32_t min_dist, std::uint32_t max_dist,
																   std::int32_t neg_expansion, std::int32_t pos_expansion) {
		// sWN LFPcubicareafilter <status:Bool1> <min:Uint32> <max:Uint32>
		//                        <neg_exp:Int32> <pos_exp:Int32>
		std::vector<std::uint8_t> params;
		params.reserve(17);
		params.push_back(enable ? 0x01 : 0x00);
		auto v = CoLaBCodec::EncodeUint32(min_dist);
		params.insert(params.end(), v.begin(), v.end());
		v = CoLaBCodec::EncodeUint32(max_dist);
		params.insert(params.end(), v.begin(), v.end());
		auto s = CoLaBCodec::EncodeInt32(neg_expansion);
		params.insert(params.end(), s.begin(), s.end());
		s = CoLaBCodec::EncodeInt32(pos_expansion);
		params.insert(params.end(), s.begin(), s.end());
		return CoLaBCodec::Encode(CommandType::kWriteByName, "LFPcubicareafilter", params);
	}


	std::vector<std::uint8_t> CommandBuilder::BuildGlossFilter(bool enable) {
		// sWN LFPglossfilter <status:Bool1>
		std::vector<std::uint8_t> params = { static_cast<std::uint8_t>(enable ? 0x01 : 0x00) };
		return CoLaBCodec::Encode(CommandType::kWriteByName, "LFPglossfilter", params);
	}

} // namespace LMS4xxx
