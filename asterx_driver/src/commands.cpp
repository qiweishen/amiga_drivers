/// @file commands.cpp
/// @brief Builds the Septentrio ASCII commands the driver needs and verifies
/// the get* readbacks of the geometry parameters. Transport/prompt/reply
/// classification live in the SsnRx SDK (3rd_party/RxTools/ssnrx); everything
/// here is pure string logic so it stays unit-testable.
/// Reference: docs/AsteRx-i3 D Pro+ Firmware v1.5.2 Reference Guide.pdf
#include "commands.hpp"

#include "string_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace asterx
{
	namespace
	{
		constexpr std::size_t kMaxAsciiCommandLength = 2000;

		using Common::StringUtil::EqualsCi;
		using Common::StringUtil::Join;
		using Common::StringUtil::SplitTrim;
		using Common::StringUtil::StartsWithCi;
		using Common::StringUtil::ToLower;
		using Common::StringUtil::Trim;


		std::string fmt_command_decimal(double v)
		{
			if (std::fabs(v) < 0.0005)
			{
				v = 0.0;
			}
			std::ostringstream os;
			os << std::fixed << std::setprecision(3) << v;
			return os.str();
		}


		int parse_int_prefix(const std::string& s, const std::string& field)
		{
			std::istringstream is(Trim(s));
			int v = 0;
			if (!(is >> v))
			{
				throw ConfigError("could not parse integer field '" + field + "' from '" + s + "'");
			}
			return v;
		}


		double parse_double_prefix(const std::string& s, const std::string& field)
		{
			std::istringstream is(Trim(s));
			double v = 0.0;
			if (!(is >> v))
			{
				throw ConfigError("could not parse numeric field '" + field + "' from '" + s + "'");
			}
			return v;
		}


		bool close_enough(double a, double b)
		{
			return std::fabs(a - b) <= 0.001;
		}


		std::string find_config_line_payload(const std::string& reply, const std::string& key)
		{
			std::istringstream lines(reply);
			std::string line;
			const std::string prefix = key + ",";
			while (std::getline(lines, line))
			{
				line = Trim(line);
				if (StartsWithCi(line, prefix))
				{
					return Trim(line.substr(prefix.size()));
				}
			}
			throw ConfigError("receiver reply did not contain '" + key + "' line: " + reply);
		}


		std::string compact_payload_after_key(const std::string& reply, const std::string& key)
		{
			const std::string marker = key + ",";
			const auto pos = reply.find(marker);
			if (pos == std::string::npos)
			{
				throw ConfigError("receiver reply did not contain '" + key + "' payload: " + reply);
			}

			std::string payload = reply.substr(pos + marker.size());
			const auto prompt = payload.find('>');
			if (prompt != std::string::npos)
			{
				payload = payload.substr(0, prompt);
			}
			for (char& c : payload)
			{
				if (c == '\r' || c == '\n' || c == '\t') c = ' ';
			}
			return payload;
		}


		void enforce_command_length(const std::string& cmd)
		{
			if (cmd.size() > kMaxAsciiCommandLength)
			{
				throw ConfigError("ASCII command exceeds Septentrio 2000-character limit");
			}
		}


		void enforce_descriptor(const std::string& descriptor)
		{
			if (descriptor.empty() ||
				descriptor.find(',') != std::string::npos ||
				descriptor.find(' ') != std::string::npos)
			{
				throw ConfigError("invalid connection descriptor '" + descriptor + "'");
			}
		}


		std::string set_vec3_command(const std::string& name, Vec3 v)
		{
			return name + ", " + fmt_command_decimal(v.x) + ", " +
				fmt_command_decimal(v.y) + ", " + fmt_command_decimal(v.z);
		}
	} // namespace


	bool is_valid_sbf_interval(const std::string& interval)
	{
		static const std::unordered_set<std::string> allowed{
			"onchange",
			"off",
			"msec5", "msec10", "msec20", "msec40", "msec50",
			"msec100", "msec200", "msec500",
			"sec1", "sec2", "sec5", "sec10", "sec15", "sec30", "sec60",
			"min2", "min5", "min10", "min15", "min30", "min60",
		};
		return allowed.find(ToLower(interval)) != allowed.end();
	}


	bool is_valid_nmea_message(const std::string& message)
	{
		// Message column of the setNMEAOutput table (firmware reference guide);
		// note that common sentences absent there (e.g. GSV) are NOT supported.
		static const std::unordered_set<std::string> allowed{
			"gga", "gll", "gns", "gst", "hdt", "rmc",
			"vtg", "zda", "hrp", "ths", "pashr",
		};
		return allowed.find(ToLower(message)) != allowed.end();
	}


	bool is_valid_pin_descriptor(const std::string& descriptor)
	{
		// Cd column of the setNMEAOutput table (firmware reference guide).
		static const std::unordered_set<std::string> allowed{
			"com1", "com2", "com3", "com4",
			"usb1", "usb2",
			"ip10", "ip11", "ip12", "ip13", "ip14", "ip15", "ip16", "ip17",
			"ntr1", "ntr2", "ntr3",
			"ips1", "ips2", "ips3", "ips4", "ips5",
			"ipr1", "ipr2", "ipr3", "ipr4", "ipr5",
			"dsk1",
		};
		return allowed.find(ToLower(descriptor)) != allowed.end();
	}


	std::string build_nmea_output_command(const NmeaPinStream& stream)
	{
		if (stream.stream_id < 1 || stream.stream_id > 10)
		{
			throw ConfigError("NMEA stream id must be in range 1..10");
		}
		if (!is_valid_pin_descriptor(stream.descriptor))
		{
			throw ConfigError("Unsupported NMEA pin descriptor '" + stream.descriptor + "'");
		}
		if (stream.messages.empty())
		{
			throw ConfigError("NMEA pin stream must contain at least one message");
		}
		for (const auto& m : stream.messages)
		{
			if (!is_valid_nmea_message(m))
			{
				throw ConfigError("Unsupported NMEA message '" + m + "'");
			}
		}
		if (!is_valid_sbf_interval(stream.interval))
		{
			throw ConfigError("Unsupported NMEA interval '" + stream.interval + "'");
		}

		const std::string cmd =
			"setNMEAOutput, Stream" + std::to_string(stream.stream_id) +
			", " + stream.descriptor +
			", " + Join(stream.messages, '+') +
			", " + stream.interval;
		enforce_command_length(cmd);
		return cmd;
	}


	std::string build_sbf_output_command(const SbfStream& stream,
	                                     const std::string& descriptor)
	{
		if (stream.stream_id < 1 || stream.stream_id > 10)
		{
			throw ConfigError("SBF stream id must be in range 1..10");
		}
		enforce_descriptor(descriptor);
		if (stream.blocks.empty())
		{
			throw ConfigError("SBF stream must contain at least one block");
		}
		if (!is_valid_sbf_interval(stream.interval))
		{
			throw ConfigError("Unsupported SBF interval '" + stream.interval + "'");
		}

		const std::string cmd =
			"setSBFOutput, Stream" + std::to_string(stream.stream_id) +
			", " + descriptor +
			", " + Join(stream.blocks, '+') +
			", " + stream.interval;
		enforce_command_length(cmd);
		return cmd;
	}


	std::string build_ins_ant_lever_arm_command(Vec3 lever_arm_m)
	{
		return set_vec3_command("setINSAntLeverArm", lever_arm_m);
	}


	std::string build_imu_orientation_command(const ReceiverSettings& settings)
	{
		if (EqualsCi(settings.imu_orientation_mode, "SensorDefault"))
		{
			return "setIMUOrientation, SensorDefault";
		}
		return "setIMUOrientation, " + settings.imu_orientation_mode + ", " +
			fmt_command_decimal(settings.theta_x_deg) + ", " +
			fmt_command_decimal(settings.theta_y_deg) + ", " +
			fmt_command_decimal(settings.theta_z_deg);
	}


	std::vector<Command> build_command_list(const ReceiverSettings& settings,
	                                        const std::string& descriptor)
	{
		enforce_descriptor(descriptor);

		std::vector<Command> cmds;
		cmds.reserve(32);

		// 1) Authenticate. SsnRx classifies the "$R?" reply, so a refused login
		//    surfaces as a plain command error.
		cmds.push_back({
			"login, " + settings.user + ", " + settings.password,
			CommandKind::Plain
		});

		// 2) Check capabilities before configuring dual-antenna collection.
		cmds.push_back({"getReceiverCapabilities", CommandKind::CheckCapabilities});

		// 3) Wipe all persistent SBF and NMEA output streams so stale streams
		//    (e.g. left behind by an RxControl session, or a pin stream removed
		//    from the config) do not duplicate epochs, keep old rates alive, or
		//    keep spraying sentences on a pin. The receiver may answer "not
		//    configured" — tolerated.
		for (int stream_id = 1; stream_id <= 10; ++stream_id)
		{
			cmds.push_back({
				"setSBFOutput, Stream" + std::to_string(stream_id) +
				", none, none, off",
				CommandKind::ToleratedError
			});
		}
		for (int stream_id = 1; stream_id <= 10; ++stream_id)
		{
			cmds.push_back({
				"setNMEAOutput, Stream" + std::to_string(stream_id) +
				", none, none, off",
				CommandKind::ToleratedError
			});
		}

		// 4) Enable SBF output on our own connection.
		// The empty middle field leaves the input direction (command entry) unchanged.
		cmds.push_back({"setDataInOut, " + descriptor + ", , +SBF", CommandKind::Plain});

		// 5) Reset tracking/usage filters so all supported observables can be
		//    generated. setSignalTracking restarts tracking loops, so do this
		//    before enabling output streams.
		if (settings.configure_all_tracking)
		{
			cmds.push_back({"setSatelliteTracking, all", CommandKind::Plain});
			cmds.push_back({"setSignalTracking, all", CommandKind::Plain});
			cmds.push_back({"setSignalUsage, all, all", CommandKind::Plain});
			cmds.push_back({
				"setCN0Mask, all, " + std::to_string(settings.cn0_mask_dbhz),
				CommandKind::Plain
			});
		}

		// 6) IMU startup mode, orientation, and antenna lever arm — each set is
		//    read back and verified: a silently wrong geometry parameter corrupts
		//    every dataset recorded with it.
		cmds.push_back({
			"setIMUStartupDataMode, " + settings.imu_startup_data_mode,
			CommandKind::Plain
		});
		cmds.push_back({build_imu_orientation_command(settings), CommandKind::Plain});
		cmds.push_back({"getIMUOrientation", CommandKind::VerifyImuOrientation});

		cmds.push_back({
			build_ins_ant_lever_arm_command(settings.ant_lever_arm_m),
			CommandKind::Plain
		});
		cmds.push_back({"getINSAntLeverArm", CommandKind::VerifyLeverArm});

		// 7) Dual-antenna GNSS attitude setup and offset confirmation.
		cmds.push_back({
			"setGNSSAttitude, " + settings.gnss_attitude_mode,
			CommandKind::Plain
		});
		cmds.push_back({"getGNSSAttitude", CommandKind::VerifyGnssAttitude});

		cmds.push_back({
			"setAttitudeOffset, " +
			fmt_command_decimal(settings.attitude_offset_deg.heading_deg) +
			", " +
			fmt_command_decimal(settings.attitude_offset_deg.pitch_deg),
			CommandKind::Plain
		});
		cmds.push_back({"getAttitudeOffset", CommandKind::VerifyAttitudeOffset});

		// 8) SBF stream membership + intervals, targeted at our own connection.
		for (const auto& s : settings.streams)
		{
			if (s.blocks.empty()) continue;
			cmds.push_back({build_sbf_output_command(s, descriptor), CommandKind::Plain});
		}

		// 9) NMEA sentence output on fixed receiver ports. Enable the NMEA
		//    output direction on each target port once (the empty middle field
		//    leaves the input direction unchanged), then attach its streams.
		//    Unlike the SBF streams these target physical pins, not our own
		//    connection, so they keep running if our session drops.
		std::vector<std::string> nmea_ports;
		for (const auto& s : settings.pin_streams)
		{
			if (s.messages.empty()) continue;
			const bool port_enabled = std::any_of(
				nmea_ports.begin(), nmea_ports.end(),
				[&s](const std::string& p) { return EqualsCi(p, s.descriptor); });
			if (!port_enabled)
			{
				nmea_ports.push_back(s.descriptor);
				cmds.push_back({
					"setDataInOut, " + s.descriptor + ", , +NMEA",
					CommandKind::Plain
				});
			}
			cmds.push_back({build_nmea_output_command(s), CommandKind::Plain});
		}

		return cmds;
	}


	ReceiverCapabilities parse_receiver_capabilities_reply(const std::string& reply)
	{
		const std::string payload = compact_payload_after_key(reply, "ReceiverCapabilities");
		const auto fields = SplitTrim(payload, ',');
		if (fields.size() < 7)
		{
			throw ConfigError("ReceiverCapabilities reply had too few fields: " + reply);
		}

		ReceiverCapabilities caps;
		for (const auto& antenna : SplitTrim(fields[0], '+'))
		{
			if (EqualsCi(antenna, "Main"))
			{
				caps.has_main = true;
			}
			if (EqualsCi(antenna, "Aux1"))
			{
				caps.has_aux1 = true;
			}
		}

		caps.measurement_interval_ms = parse_int_prefix(fields[fields.size() - 3], "measurement_interval_ms");
		caps.pvt_interval_ms = parse_int_prefix(fields[fields.size() - 2], "pvt_interval_ms");
		caps.ins_interval_ms = parse_int_prefix(fields[fields.size() - 1], "ins_interval_ms");
		return caps;
	}


	void verify_imu_orientation_reply(const std::string& reply, const ReceiverSettings& settings)
	{
		const auto fields = SplitTrim(find_config_line_payload(reply, "IMUOrientation"), ',');
		if (fields.empty())
		{
			throw ConfigError("IMUOrientation reply did not contain an orientation mode");
		}
		if (!EqualsCi(fields[0], settings.imu_orientation_mode))
		{
			throw ConfigError("IMU orientation mismatch: expected " +
				settings.imu_orientation_mode + ", got " + fields[0]);
		}
		if (!EqualsCi(settings.imu_orientation_mode, "SensorDefault"))
		{
			if (fields.size() < 4)
			{
				throw ConfigError("IMUOrientation reply did not include theta values");
			}
			const double theta_x = parse_double_prefix(fields[1], "ThetaX");
			const double theta_y = parse_double_prefix(fields[2], "ThetaY");
			const double theta_z = parse_double_prefix(fields[3], "ThetaZ");
			if (!close_enough(theta_x, settings.theta_x_deg) ||
				!close_enough(theta_y, settings.theta_y_deg) ||
				!close_enough(theta_z, settings.theta_z_deg))
			{
				throw ConfigError("IMU orientation theta values do not match requested config");
			}
		}
	}


	void verify_ins_ant_lever_arm_reply(const std::string& reply, Vec3 expected)
	{
		const auto fields = SplitTrim(find_config_line_payload(reply, "INSAntLeverArm"), ',');
		if (fields.size() < 3)
		{
			throw ConfigError("INSAntLeverArm reply did not contain x/y/z values");
		}
		const Vec3 actual{
			parse_double_prefix(fields[0], "INSAntLeverArm.X"),
			parse_double_prefix(fields[1], "INSAntLeverArm.Y"),
			parse_double_prefix(fields[2], "INSAntLeverArm.Z"),
		};
		if (!close_enough(actual.x, expected.x) ||
			!close_enough(actual.y, expected.y) ||
			!close_enough(actual.z, expected.z))
		{
			throw ConfigError("INS antenna lever arm does not match requested config");
		}
	}


	void verify_gnss_attitude_reply(const std::string& reply, const std::string& expected_mode)
	{
		const auto fields = SplitTrim(find_config_line_payload(reply, "GNSSAttitude"), ',');
		if (fields.empty())
		{
			throw ConfigError("GNSSAttitude reply did not contain a mode");
		}
		if (!EqualsCi(fields[0], expected_mode))
		{
			throw ConfigError("GNSS attitude mode mismatch: expected " + expected_mode + ", got " + fields[0]);
		}
	}


	void verify_attitude_offset_reply(const std::string& reply, AttitudeOffset expected)
	{
		const auto fields = SplitTrim(find_config_line_payload(reply, "AttitudeOffset"), ',');
		if (fields.size() < 2)
		{
			throw ConfigError("AttitudeOffset reply did not contain heading/pitch values");
		}
		const double heading = parse_double_prefix(fields[0], "AttitudeOffset.Heading");
		const double pitch = parse_double_prefix(fields[1], "AttitudeOffset.Pitch");
		if (!close_enough(heading, expected.heading_deg) ||
			!close_enough(pitch, expected.pitch_deg))
		{
			throw ConfigError("GNSS attitude offset does not match requested config");
		}
	}


	void validate_receiver_settings(const ReceiverSettings& settings)
	{
		if (!EqualsCi(settings.imu_startup_data_mode, "Boot") &&
			!EqualsCi(settings.imu_startup_data_mode, "GnssTimeKnown"))
		{
			throw ConfigError("receiver.imu.startup_data_mode must be Boot or GnssTimeKnown");
		}
		if (!EqualsCi(settings.imu_orientation_mode, "SensorDefault") &&
			!EqualsCi(settings.imu_orientation_mode, "manual") &&
			!EqualsCi(settings.imu_orientation_mode, "fixed"))
		{
			throw ConfigError("receiver.imu.orientation_mode must be SensorDefault, manual, or fixed");
		}
		if (!settings.ant_lever_arm_configured)
		{
			throw ConfigError("receiver.imu.ant_lever_arm_m is required");
		}
		const auto in_lever_range = [](double v) { return v >= -100.0 && v <= 100.0; };
		if (!in_lever_range(settings.ant_lever_arm_m.x) ||
			!in_lever_range(settings.ant_lever_arm_m.y) ||
			!in_lever_range(settings.ant_lever_arm_m.z))
		{
			throw ConfigError("receiver.imu.ant_lever_arm_m components must be in [-100, 100] meters");
		}
		if (!EqualsCi(settings.gnss_attitude_mode, "none") &&
			!EqualsCi(settings.gnss_attitude_mode, "MultiAntenna"))
		{
			throw ConfigError("receiver.gnss_attitude.mode must be none or MultiAntenna");
		}
		if (settings.cn0_mask_dbhz < 0 || settings.cn0_mask_dbhz > 60)
		{
			throw ConfigError("receiver.tracking.cn0_mask_dbhz must be in range 0..60 dB-Hz");
		}
		if (settings.streams.empty())
		{
			throw ConfigError("receiver.streams must contain at least one stream");
		}
		for (const auto& s : settings.streams)
		{
			// Validate with a placeholder descriptor; the real one is only known
			// once connected.
			(void)build_sbf_output_command(s, "IP10");
		}
		std::unordered_set<int> pin_stream_ids;
		for (const auto& s : settings.pin_streams)
		{
			// A duplicate id would silently overwrite the earlier stream on the
			// receiver.
			if (!pin_stream_ids.insert(s.stream_id).second)
			{
				throw ConfigError("receiver.pin_streams contains duplicate stream id " +
					std::to_string(s.stream_id));
			}
			(void)build_nmea_output_command(s);
		}
	}


	std::string redact_cmd(const std::string& cmd)
	{
		if (cmd.rfind("login", 0) == 0)
		{
			return "login, <REDACTED>, <REDACTED>";
		}
		return cmd;
	}
} // namespace asterx
