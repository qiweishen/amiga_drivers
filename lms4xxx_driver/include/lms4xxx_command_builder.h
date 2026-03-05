#ifndef LMS4XXX_COMMAND_BUILDER_H
#define LMS4XXX_COMMAND_BUILDER_H

#include <array>
#include <cstdint>
#include <vector>

#include "lms4xxx_config.h"


namespace LMS4xxx {

	// Builds CoLa B telegrams for LMS4xxx device commands.
	class CommandBuilder {
	public:
		CommandBuilder() = default;

		// Build "sMN SetAccessMode" telegram for Authorized Client login.
		[[nodiscard]] static std::vector<std::uint8_t> BuildLogin();

		// Build "sMN Run" telegram to activate parameter changes and logout.
		[[nodiscard]] static std::vector<std::uint8_t> BuildRun();

		// Build "sWN LMDscandatacfg" telegram.
		[[nodiscard]] static std::vector<std::uint8_t> BuildScanDataConfig(const ScanConfig &config);

		// Build "sWN LMPoutputRange" telegram.
		[[nodiscard]] static std::vector<std::uint8_t> BuildOutputRange(const ScanConfig &config);

		// Build "sEN LMDscandata 1" to start continuous scan data streaming.
		[[nodiscard]] static std::vector<std::uint8_t> BuildStartStream();

		// Build "sEN LMDscandata 0" to stop continuous scan data streaming.
		[[nodiscard]] static std::vector<std::uint8_t> BuildStopStream();

		// Build "sRN LMDscandata" to poll a single scan frame.
		[[nodiscard]] static std::vector<std::uint8_t> BuildPollScan();

		// Build "sMN LMCstartmeas" to start laser measurement.
		[[nodiscard]] static std::vector<std::uint8_t> BuildStartMeasurement();

		// Build "sMN LMCstopmeas" to stop laser measurement.
		[[nodiscard]] static std::vector<std::uint8_t> BuildStopMeasurement();

		// Build "sMN LMCstandby" to enter standby mode.
		[[nodiscard]] static std::vector<std::uint8_t> BuildStandby();

		// Build "sMN mSCreboot" to reboot the device.
		[[nodiscard]] static std::vector<std::uint8_t> BuildReboot();

		// Build "sMN mEEwriteall" to permanently save parameters.
		[[nodiscard]] static std::vector<std::uint8_t> BuildSaveParams();

		// Build "sWN TSCRole" to set time sync role (0=Off, 1=Client, 2=Server).
		[[nodiscard]] static std::vector<std::uint8_t> BuildSetTimeSyncRole(std::uint8_t role);

		// Build "sMN LSPsetdatetime" to set the device clock.
		[[nodiscard]] static std::vector<std::uint8_t> BuildSetDatetime(std::uint16_t year, std::uint8_t month, std::uint8_t day,
																		std::uint8_t hour, std::uint8_t minute, std::uint8_t second,
																		std::uint32_t microsecond);

		// Build "sWN TSCTCSrvAddr" to set NTP server address.
		[[nodiscard]] static std::vector<std::uint8_t> BuildSetNtpServer(const std::array<std::uint8_t, 4> &ip_bytes);

		// Build "sWN TSCTCupdatetime" to set NTP update interval.
		[[nodiscard]] static std::vector<std::uint8_t> BuildSetNtpUpdateTime(std::uint32_t seconds);

		// Build "sWN TSCTCtimezone" to set time zone.
		[[nodiscard]] static std::vector<std::uint8_t> BuildSetNtpTimezone(std::uint8_t timezone);

		// Build "sWN LFPmeanfilter".
		[[nodiscard]] static std::vector<std::uint8_t> BuildMeanFilter(bool enable, std::uint16_t num_scans);

		// Build "sWN LFPmedianfilter".
		[[nodiscard]] static std::vector<std::uint8_t> BuildMedianFilter(bool enable);

		// Build "sWN LFPfrontendEdgefilter".
		[[nodiscard]] static std::vector<std::uint8_t> BuildFrontendEdgeFilter(bool enable);

		// Build "sWN LFPedgefilter".
		[[nodiscard]] static std::vector<std::uint8_t> BuildEdgeFilter(bool enable);

		// Build "sWN LFPcubicareafilter".
		[[nodiscard]] static std::vector<std::uint8_t> BuildCubicAreaFilter(bool enable, std::uint32_t min_dist,
																			std::uint32_t max_dist, std::int32_t neg_expansion,
																			std::int32_t pos_expansion);

		// Build "sWN LFPglossfilter".
		[[nodiscard]] static std::vector<std::uint8_t> BuildGlossFilter(bool enable);

	private:
		// Authorized Client login -> Refer to manual page 72 (level 3, password F4724744)
		static constexpr std::uint8_t kAuthorizedClientLevel = 0x03;
		static constexpr std::uint32_t kAuthorizedClientPassword = 0xF4724744;
	};

}  // namespace LMS4xxx

#endif // LMS4XXX_COMMAND_BUILDER_H
