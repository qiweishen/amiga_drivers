#include "lms4xxx_driver.h"

#include <array>
#include <boost/asio/ip/address_v4.hpp>
#include <chrono>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <mutex>
#include <string_view>
#include <thread>

#include "lms4xxx_cola_b.h"
#include "lms4xxx_command_builder.h"
#include "lms4xxx_error.h"
#include "lms4xxx_frame_receiver.h"
#include "lms4xxx_scan_data_parser.h"
#include "lms4xxx_spsc_ring_buffer.h"
#include "lms4xxx_tcp_client.h"
#include "logger.h"
#include "thread_util.h"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxDriver";

	Common::DriverLog g_log{ std::string(kModule) };

	// Read buffer size for the receive thread (16 KB).
	constexpr std::size_t kReadBufferSize = 16 * 1024;

	// Parse thread backoff sleep when ring buffer is empty.
	constexpr auto kParseBackoffSleep = std::chrono::microseconds(100);
}  // namespace


namespace LMS4xxx {

	struct LMS4xxxDriver::Impl {
		DriverConfig config;

		// --- Components ---
		std::unique_ptr<TCPClient> tcp_client;
		std::unique_ptr<FrameReceiver> frame_receiver;
		std::unique_ptr<FrameRingBuffer> ring_buffer;

		// --- State ---
		std::atomic<ConnectionState> state{ ConnectionState::kDisconnected };
		std::atomic<bool> scanning{ false };
		std::atomic<bool> receive_running{ false };
		std::atomic<bool> parse_running{ false };

		// --- Threads ---
		std::thread receive_thread;
		std::thread parse_thread;

		// --- Callbacks ---
		ScanDataCallback scan_callback;
		ConnectionStateCallback connection_callback;
		ErrorCallback error_callback;
		std::mutex callback_mutex;	// Protects callback registration (not invocation)

		// --- Statistics ---
		DriverStatistics stats;

		// --- Response handling for command/response phase ---
		std::mutex response_mutex;
		std::vector<std::uint8_t> response_buffer;

		explicit Impl(const DriverConfig &cfg) : config(cfg) {}

		// Set connection state and invoke callback.
		void set_state(ConnectionState new_state) {
			state.store(new_state, std::memory_order_release);
			ConnectionStateCallback cb;
			{
				std::lock_guard lock(callback_mutex);
				cb = connection_callback;
			}
			if (cb) {
				try {
					cb(new_state);
				} catch (const std::exception &e) {
					g_log.warn("Connection callback threw - {}", e.what());
				}
			}
		}

		// Invoke error callback.
		void report_error(std::error_code ec, const std::string &detail = "") {
			ErrorCallback cb;
			{
				std::lock_guard lock(callback_mutex);
				cb = error_callback;
			}
			if (cb) {
				try {
					cb(ec, detail);
				} catch (const std::exception &e) {
					g_log.warn("Error callback threw - {}", e.what());
				}
			}
		}

		// Send a CoLa B frame and read a synchronous response
		// Used during the command/response phase (connect, configure)
		[[nodiscard]] std::error_code send_and_receive(const std::vector<std::uint8_t> &frame, CoLaBMessage &response,
													   int timeout_ms) {
			if (!tcp_client || !tcp_client->IsConnected()) {
				return make_error_code(ErrorCode::kNotConnected);
			}

			auto ec = tcp_client->Write(frame);
			if (ec) {
				return ec;
			}

			// Read response frame synchronously.
			// Frame format: STX(4) + Length(4) + Data(N) + Checksum(1)

			// 1. Read STX (4 bytes)
			std::uint8_t header[8];
			std::error_code read_ec;
			auto bytes = tcp_client->Read(header, 8, read_ec, timeout_ms);
			if (read_ec)
				return read_ec;
			if (bytes < 8)
				return make_error_code(ErrorCode::kFrameTooShort);

			// Verify STX
			if (header[0] != 0x02 || header[1] != 0x02 || header[2] != 0x02 || header[3] != 0x02) {
				g_log.warn("Invalid STX in response: {:02X} {:02X} {:02X} {:02X}", header[0], header[1], header[2], header[3]);
				return make_error_code(ErrorCode::kProtocolError);
			}

			// 2. Read data length (big-endian uint32)
			const auto data_len = CoLaBCodec::DecodeUint32(header + 4);

			if (data_len > 64 * 1024) {
				g_log.warn("Response data length {} exceeds max", data_len);
				return make_error_code(ErrorCode::kFrameTooLong);
			}

			// 3. Read Data + Checksum
			std::vector<std::uint8_t> data_and_cs(data_len + 1);
			bytes = tcp_client->Read(data_and_cs.data(), data_and_cs.size(), read_ec, timeout_ms);
			if (read_ec) {
				return read_ec;
			}
			if (bytes < data_and_cs.size()) {
				return make_error_code(ErrorCode::kFrameTooShort);
			}

			// 4. Validate CRC8
			const auto computed_cs = CoLaBCodec::ComputeChecksum(data_and_cs.data(), data_len);
			const auto received_cs = data_and_cs[data_len];
			if (computed_cs != received_cs) {
				g_log.warn("CRC mismatch in response: computed 0x{:02X}, received 0x{:02X}", computed_cs, received_cs);
				return make_error_code(ErrorCode::kCrcError);
			}

			// 5. Decode message
			return CoLaBCodec::Decode(data_and_cs.data(), data_len, response);
		}

		// Validate a command response matches the expected command type and name.
		[[nodiscard]] std::error_code validate_response(const CoLaBMessage &msg, std::string_view expected_type,
														std::string_view expected_name) {
			if (msg.command_type != expected_type) {
				g_log.warn("Unexpected response type '{}', expected '{}'", msg.command_type, expected_type);
				return make_error_code(ErrorCode::kUnexpectedResponse);
			}
			if (msg.command_name != expected_name) {
				g_log.warn("Unexpected response name '{}', expected '{}'", msg.command_name, expected_name);
				return make_error_code(ErrorCode::kUnexpectedResponse);
			}
			return {};
		}

		// Configure the receive thread for real-time scheduling (failures are
		// expected without CAP_SYS_NICE, e.g. inside Docker: warn and degrade).
		void configure_receive_thread() {
			if (config.network.receive_thread_priority > 0) {
				const int prio = config.network.receive_thread_priority;
				if (const int ret = Common::ThreadUtil::SetRealtimePriority(receive_thread, prio); ret != 0) {
					g_log.warn("Failed to set SCHED_FIFO priority {}: {} (requires root or CAP_SYS_NICE)", prio, std::strerror(ret));
				} else {
					g_log.trace("Receive thread: SCHED_FIFO priority {}", prio);
				}
			}

			if (config.network.receive_thread_cpu >= 0) {
				const int cpu = config.network.receive_thread_cpu;
				if (const int ret = Common::ThreadUtil::PinToCpu(receive_thread, cpu); ret != 0) {
					g_log.warn("Failed to set CPU affinity to core {}: {}", cpu, std::strerror(ret));
				} else {
					g_log.trace("Receive thread pinned to CPU {}", cpu);
				}
			}
		}

		// Receive thread main loop.
		void receive_loop() {
			g_log.trace("Receive thread started");

			std::vector<std::uint8_t> buf(kReadBufferSize);

			while (receive_running.load(std::memory_order_acquire)) {
				std::error_code ec;
				const auto n = tcp_client->ReadSome(buf.data(), buf.size(), ec);

				if (ec) {
					if (receive_running.load(std::memory_order_relaxed)) {
						g_log.error("Receive thread: read error - {}", ec.message());
						report_error(ec, "receive thread read error");
						set_state(ConnectionState::kError);
					}
					break;
				}

				if (n > 0) {
					stats.bytes_received.fetch_add(n, std::memory_order_relaxed);
					frame_receiver->Feed(buf.data(), n);
				}
			}

			g_log.trace("Receive thread stopped");
		}


		// Parse thread main loop.
		void parse_loop() {
			g_log.trace("Parse thread started");

			bool first_frame = true;

			while (parse_running.load(std::memory_order_acquire)) {
				RawFrame frame;
				if (!ring_buffer->try_pop(frame)) {
					// No frames available — brief backoff.
					std::this_thread::sleep_for(kParseBackoffSleep);
					continue;
				}

				CoLaBMessage msg;
				auto ec = CoLaBCodec::Decode(frame.data.data(), frame.data.size(), msg);
				if (ec) {
					stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
					g_log.warn("Frame decode error - {}", ec.message());
					continue;
				}

				// Only process scan data notifications.
				if (msg.command_name != "LMDscandata") {
					continue;
				}

				ScanData scan;
				ec = ScanDataParser::Parse(msg.payload.data(), msg.payload.size(), scan);
				if (ec) {
					stats.parse_errors.fetch_add(1, std::memory_order_relaxed);
					g_log.warn("Scan data parse error - {}", ec.message());
					continue;
				}

				stats.frames_parsed.fetch_add(1, std::memory_order_relaxed);

				// Check telegram counter continuity.
				if (!first_frame) {
					const auto prev_counter = stats.last_telegram_counter.load(std::memory_order_relaxed);
					const auto expected = static_cast<std::uint32_t>((prev_counter + 1) & 0xFFFF);
					if (scan.telegram_counter != expected) {
						const auto gap = (scan.telegram_counter >= expected) ? scan.telegram_counter - expected
																			 : (0x10000 + scan.telegram_counter - expected);
						stats.counter_gaps.fetch_add(1, std::memory_order_relaxed);
						g_log.warn("Telegram counter gap: expected {}, got {} (missed ~{} frames)", expected, scan.telegram_counter, gap);
					}
				}
				first_frame = false;

				stats.last_telegram_counter.store(scan.telegram_counter, std::memory_order_relaxed);
				stats.last_scan_counter.store(scan.scan_counter, std::memory_order_relaxed);
				stats.last_frame_time_us.store(frame.receive_timestamp_us, std::memory_order_relaxed);

				ScanDataCallback cb;
				{
					std::lock_guard lock(callback_mutex);
					cb = scan_callback;
				}
				if (cb) {
					try {
						cb(scan);
					} catch (const std::exception &e) {
						g_log.warn("Scan callback threw - {}", e.what());
					}
				}
			}

			g_log.trace("Parse thread stopped");
		}
	};


	LMS4xxxDriver::LMS4xxxDriver(const DriverConfig &config) : impl_(std::make_unique<Impl>(config)) {}

	LMS4xxxDriver::~LMS4xxxDriver() {
		if (impl_->scanning.load(std::memory_order_relaxed)) {
			StopScanning();
		}
		if (impl_->tcp_client && impl_->tcp_client->IsConnected()) {
			Disconnect();
		}
	}


	std::error_code LMS4xxxDriver::Connect() {
		auto ec = impl_->config.Validate();
		if (ec) {
			return ec;
		}

		impl_->set_state(ConnectionState::kConnecting);

		impl_->tcp_client = std::make_unique<TCPClient>(MakeTcpOptions(impl_->config.device, impl_->config.network));

		ec = impl_->tcp_client->Connect(impl_->config.network.connect_timeout_ms);
		if (ec) {
			impl_->set_state(ConnectionState::kError);
			impl_->report_error(ec, "TCP connect failed");
			return ec;
		}

		impl_->set_state(ConnectionState::kConnected);
		g_log.trace("Connected to {}", impl_->tcp_client->RemoteEndpointStr());
		return {};
	}


	std::error_code LMS4xxxDriver::Configure() {
		if (!impl_->tcp_client || !impl_->tcp_client->IsConnected()) {
			return make_error_code(ErrorCode::kNotConnected);
		}

		impl_->set_state(ConnectionState::kConfiguring);
		const int timeout = impl_->config.network.response_timeout_ms;

		// Following the workflow documented on Page 69 of the Operating Instructions
		// 1. Log in as Authorized Client
		{
			auto frame = CommandBuilder::BuildLogin();
			CoLaBMessage response;
			auto ec = impl_->send_and_receive(frame, response, timeout);
			if (ec) {
				g_log.error("Login failed - {}", ec.message());
				impl_->set_state(ConnectionState::kError);
				return ec;
			}
			ec = impl_->validate_response(response, CommandType::kMethodAnswer, "SetAccessMode");
			if (ec) {
				return ec;
			}

			// Check response: payload[0] should be 0x01 (success)
			if (response.payload.empty() || response.payload[0] != 0x01) {
				g_log.error("Login rejected by device");
				impl_->set_state(ConnectionState::kError);
				return make_error_code(ErrorCode::kAccessDenied);
			}
			g_log.trace("Logged in as Authorized Client");
		}


		// 2. Configure scandata content
		{
			auto frame = CommandBuilder::BuildScanDataConfig(impl_->config.scan);
			CoLaBMessage response;
			auto ec = impl_->send_and_receive(frame, response, timeout);
			if (ec) {
				g_log.error("Set scan data config failed - {}", ec.message());
				impl_->set_state(ConnectionState::kError);
				return ec;
			}
			ec = impl_->validate_response(response, CommandType::kWriteAnswer, "LMDscandatacfg");
			if (ec)
				return ec;
			g_log.trace("Scan data configuration set");
		}


		// 3. Configure scandata output
		{
			auto frame = CommandBuilder::BuildOutputRange(impl_->config.scan);
			CoLaBMessage response;
			auto ec = impl_->send_and_receive(frame, response, timeout);
			if (ec) {
				g_log.error("Set output range failed - {}", ec.message());
				impl_->set_state(ConnectionState::kError);
				return ec;
			}
			ec = impl_->validate_response(response, CommandType::kWriteAnswer, "LMPoutputRange");
			if (ec)
				return ec;
			g_log.trace("Output range set: {:.4f}° to {:.4f}° @ {:.4f}° resolution", impl_->config.scan.start_angle_deg,
						impl_->config.scan.stop_angle_deg, impl_->config.scan.angular_resolution_deg);
		}


		// 4. Parameter settings before store parameters
		// 4.1. Always disable all filters (hardcoded)
		{
			auto cmds = {
				CommandBuilder::BuildMeanFilter(false, 2),
				CommandBuilder::BuildMedianFilter(false),
				CommandBuilder::BuildFrontendEdgeFilter(false),
				CommandBuilder::BuildEdgeFilter(false),
				CommandBuilder::BuildCubicAreaFilter(false, 0, 91776, 0, 0),
				CommandBuilder::BuildGlossFilter(false),
			};
			for (const auto &frame: cmds) {
				CoLaBMessage response;
				auto ec = impl_->send_and_receive(frame, response, timeout);
				if (ec) {
					g_log.warn("Filter disable command failed - {}", ec.message());
				}
			}
			g_log.trace("All filters disabled");
		}


		// 4.2. Configure NTP (if enabled)
		if (impl_->config.ntp.enable) {
			// Send an NTP sWN command, validate the sWA response.
			// Returns true on success, false on transport error or device rejection (sFA).
			auto ntp_write = [&](const std::vector<std::uint8_t> &frame, std::string_view cmd_name, const char *desc) -> bool {
				CoLaBMessage response;
				auto ec = impl_->send_and_receive(frame, response, timeout);
				if (ec) {
					g_log.warn("{} failed: {}", desc, ec.message());
					return false;
				}
				ec = impl_->validate_response(response, CommandType::kWriteAnswer, cmd_name);
				if (ec) {
					g_log.warn("{} rejected (response: {} {})", desc, response.command_type, response.command_name);
					return false;
				}
				return true;
			};

			bool ntp_ok = true;

			// TSCRole
			ntp_ok = ntp_write(CommandBuilder::BuildSetTimeSyncRole(static_cast<std::uint8_t>(impl_->config.ntp.role)), "TSCRole", "Set TSCRole");

			// TSCTCSrvAddr — parse server_ip string to 4 bytes
			if (ntp_ok) {
				boost::system::error_code bec;
				const auto addr = boost::asio::ip::make_address_v4(impl_->config.ntp.server_ip, bec);
				if (!bec) {
					ntp_ok = ntp_write(CommandBuilder::BuildSetNtpServer(addr.to_bytes()), "TSCTCSrvAddr", "Set NTP server");
				} else {
					g_log.warn("Invalid NTP server IP: {}", impl_->config.ntp.server_ip);
					ntp_ok = false;
				}
			}

			// TSCTCupdatetime
			if (ntp_ok) {
				ntp_ok = ntp_write(CommandBuilder::BuildSetNtpUpdateTime(impl_->config.ntp.update_interval_s), "TSCTCupdatetime",
								   "Set NTP update time");
			}

			// TSCTCtimezone — hardcoded COORD_WORLD_TIME (34)
			if (ntp_ok) {
				ntp_ok = ntp_write(CommandBuilder::BuildSetNtpTimezone(34), "TSCTCtimezone", "Set NTP timezone");
			}

			if (ntp_ok) {
				g_log.info("NTP configured: role={}, server={}, interval={}s, timezone=UTC", static_cast<int>(impl_->config.ntp.role),
						   impl_->config.ntp.server_ip, impl_->config.ntp.update_interval_s);

				// Record the host wall-clock time of successful NTP configuration.
				struct timespec ts{};
				if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
					const auto us = static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec) / 1'000ULL;
					impl_->stats.ntp_configured_at_us.store(us, std::memory_order_relaxed);
				}
			} else {
				g_log.warn("NTP configuration failed, continuing without NTP");
				impl_->config.ntp.enable = false;
			}
		} else {
			// NTP disabled: set TSCRole=Off. mEEwriteall below will persist it.
			CoLaBMessage response;
			auto ec = impl_->send_and_receive(CommandBuilder::BuildSetTimeSyncRole(static_cast<std::uint8_t>(TscRole::kOff)), response, timeout);
			if (ec) {
				g_log.warn("Failed to disable TSCRole (non-fatal): {}", ec.message());
			}
			g_log.info("NTP disabled (Timestamp role=Off)");
		}


		// 5. Store parameters
		{
			auto frame = CommandBuilder::BuildSaveParams();
			CoLaBMessage response;
			auto ec = impl_->send_and_receive(frame, response, timeout);
			if (ec) {
				g_log.warn("mEEwriteall failed: {} (params activated but not persisted)", ec.message());
			} else {
				ec = impl_->validate_response(response, CommandType::kMethodAnswer, "mEEwriteall");
				if (ec) {
					g_log.warn("mEEwriteall unexpected response: {} {}", response.command_type, response.command_name);
				} else {
					g_log.trace("Parameters saved (mEEwriteall)");
				}
			}
		}


		// 6. Start measurement
		{
			auto frame = CommandBuilder::BuildStartMeasurement();
			CoLaBMessage response;
			auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
			if (ec) {
				g_log.error("Start measurement command failed - {}", ec.message());
				return ec;
			}
			auto validate_ec = impl_->validate_response(response, CommandType::kMethodAnswer, "LMCstartmeas");
			if (validate_ec) {
				return validate_ec;
			}

			if (response.payload.empty() || response.payload[0] != 0x00) {
				g_log.error("Start measurement rejected");
				return make_error_code(ErrorCode::kCommandRejected);
			}
		}


		// 7. Activate configuration and log out (sMN Run)
		{
			auto frame = CommandBuilder::BuildRun();
			CoLaBMessage response;
			auto ec = impl_->send_and_receive(frame, response, timeout);
			if (ec) {
				g_log.error("Run command failed - {}", ec.message());
				impl_->set_state(ConnectionState::kError);
				return ec;
			}
			ec = impl_->validate_response(response, CommandType::kMethodAnswer, "Run");
			if (ec) {
				return ec;
			}

			if (response.payload.empty() || response.payload[0] != 0x01) {
				g_log.error("Run command rejected");
				impl_->set_state(ConnectionState::kError);
				return make_error_code(ErrorCode::kCommandRejected);
			}
			g_log.trace("Configuration activated (Run)");
		}

		impl_->set_state(ConnectionState::kConnected);
		return {};
	}


	std::error_code LMS4xxxDriver::StartScanning() {
		if (!impl_->tcp_client || !impl_->tcp_client->IsConnected()) {
			return make_error_code(ErrorCode::kNotConnected);
		}
		if (impl_->scanning.load(std::memory_order_relaxed)) {
			return make_error_code(ErrorCode::kAlreadyScanning);
		}

		impl_->stats.Reset();

		impl_->ring_buffer = std::make_unique<FrameRingBuffer>(impl_->config.network.ring_buffer_frames);

		auto *stats_ptr = &impl_->stats;
		auto *ring_ptr = impl_->ring_buffer.get();

		impl_->frame_receiver = std::make_unique<FrameReceiver>(
				// on_frame: push to ring buffer
				[stats_ptr, ring_ptr](RawFrame &&frame) {
					stats_ptr->frames_received.fetch_add(1, std::memory_order_relaxed);
					if (!ring_ptr->try_push(std::move(frame))) {
						stats_ptr->frames_dropped.fetch_add(1, std::memory_order_relaxed);
					}
				},
				// on_error: count errors
				[stats_ptr](const char *reason) {
					// Distinguish CRC from framing errors by keyword.
					if (std::strstr(reason, "CRC") || std::strstr(reason, "checksum")) {
						stats_ptr->crc_errors.fetch_add(1, std::memory_order_relaxed);
					} else {
						stats_ptr->framing_errors.fetch_add(1, std::memory_order_relaxed);
					}
				});

		{
			auto frame = CommandBuilder::BuildStartStream();
			CoLaBMessage response;
			auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
			if (ec) {
				g_log.error("Start stream command failed - {}", ec.message());
				return ec;
			}
			auto validate_ec = impl_->validate_response(response, CommandType::kEventAnswer, "LMDscandata");
			if (validate_ec) {
				return validate_ec;
			}

			if (response.payload.empty() || response.payload[0] != 0x01) {
				g_log.error("Start stream rejected");
				return make_error_code(ErrorCode::kCommandRejected);
			}
		}

		impl_->scanning.store(true, std::memory_order_release);
		impl_->receive_running.store(true, std::memory_order_release);
		impl_->parse_running.store(true, std::memory_order_release);

		impl_->parse_thread = std::thread([this]() { impl_->parse_loop(); });
		impl_->receive_thread = std::thread([this]() { impl_->receive_loop(); });

		// Configure receive thread for real-time scheduling (after thread creation)
		impl_->configure_receive_thread();

		impl_->set_state(ConnectionState::kScanning);
		g_log.trace("Scanning started (ring buffer capacity: {} frames)", impl_->ring_buffer->capacity());
		return {};
	}


	std::error_code LMS4xxxDriver::StopScanning() {
		if (!impl_->scanning.load(std::memory_order_relaxed)) {
			return make_error_code(ErrorCode::kNotScanning);
		}

		g_log.trace("Stopping scan...");

		impl_->receive_running.store(false, std::memory_order_release);
		impl_->parse_running.store(false, std::memory_order_release);
		impl_->scanning.store(false, std::memory_order_release);

		// Send stop stream command BEFORE shutting down receive.
		// Full-duplex guarantees concurrent Write + ReadSome is safe.
		if (impl_->tcp_client && impl_->tcp_client->IsConnected()) {
			auto frame = CommandBuilder::BuildStopStream();
			auto ec = impl_->tcp_client->Write(frame);
			if (ec) {
				g_log.warn("Failed to send stop stream command (non-fatal) - {}", ec.message());
			}
			frame = CommandBuilder::BuildStandby();
			ec = impl_->tcp_client->Write(frame);
			if (ec) {
				g_log.warn("Failed to send standby command (non-fatal) - {}", ec.message());
			}
		}

		// Unblock the receive thread: Boost.Asio uses epoll_wait() internally for
		// synchronous read_some(), so SO_RCVTIMEO has no effect. Calling
		// shutdown(SHUT_RD) triggers POLLHUP which wakes epoll_wait() immediately,
		// causing read_some() to return EOF. The receive loop will then see
		// receive_running==false and exit cleanly.
		if (impl_->tcp_client) {
			impl_->tcp_client->ShutdownReceive();
		}

		if (impl_->receive_thread.joinable()) {
			impl_->receive_thread.join();
		}
		if (impl_->parse_thread.joinable()) {
			impl_->parse_thread.join();
		}

		impl_->set_state(ConnectionState::kConnected);
		g_log.trace("Scanning stopped");
		return {};
	}


	void LMS4xxxDriver::Disconnect() {
		if (impl_->scanning.load(std::memory_order_relaxed)) {
			StopScanning();
		}

		if (impl_->tcp_client) {
			impl_->tcp_client->Disconnect();
		}

		impl_->frame_receiver.reset();
		impl_->ring_buffer.reset();

		impl_->set_state(ConnectionState::kDisconnected);
		g_log.trace("Disconnected");
	}


	void LMS4xxxDriver::SetScanCallback(ScanDataCallback callback) {
		std::lock_guard lock(impl_->callback_mutex);
		impl_->scan_callback = std::move(callback);
	}


	void LMS4xxxDriver::SetConnectionCallback(ConnectionStateCallback callback) {
		std::lock_guard lock(impl_->callback_mutex);
		impl_->connection_callback = std::move(callback);
	}


	void LMS4xxxDriver::SetErrorCallback(ErrorCallback callback) {
		std::lock_guard lock(impl_->callback_mutex);
		impl_->error_callback = std::move(callback);
	}


	ConnectionState LMS4xxxDriver::GetConnectionState() const {
		return impl_->state.load(std::memory_order_acquire);
	}


	bool LMS4xxxDriver::IsConnected() const {
		auto s = impl_->state.load(std::memory_order_acquire);
		return s != ConnectionState::kDisconnected && s != ConnectionState::kError;
	}


	bool LMS4xxxDriver::IsScanning() const {
		return impl_->scanning.load(std::memory_order_acquire);
	}


	DriverStatistics::Snapshot LMS4xxxDriver::GetStatistics() const {
		return impl_->stats.GetSnapshot();
	}


	void LMS4xxxDriver::LogStatistics() const {
		const auto s = impl_->stats.GetSnapshot();
		g_log.info("=== LMS4xxx RECEIVING STATISTICS === : bytes={}, frames_recv={}, delivery={:.1f}%", s.bytes_received,
				   s.frames_received, s.DeliveryRate());
		g_log.info("=== LMS4xxx RECEIVING STATISTICS === : parsed={}, dropped={}, counter_gaps={}", s.frames_parsed, s.frames_dropped,
				   s.counter_gaps);
		g_log.info("=== LMS4xxx RECEIVING STATISTICS === : crc_err={}, framing_err={}, parse_err={}", s.crc_errors, s.framing_errors,
				   s.parse_errors);
		if (s.ntp_configured_at_us != 0) {
			const auto sec = static_cast<std::time_t>(s.ntp_configured_at_us / 1'000'000ULL);
			char buf[32]{};
			struct tm tm_info{};
			gmtime_r(&sec, &tm_info);
			std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_info);
			g_log.info("=== LMS4xxx RECEIVING STATISTICS === : NTP configured at host time: {}", buf);
		}
	}


	std::error_code LMS4xxxDriver::PollSingleScan(ScanData &out) {
		if (impl_->scanning.load(std::memory_order_relaxed)) {
			return make_error_code(ErrorCode::kAlreadyScanning);
		}

		auto frame = CommandBuilder::BuildPollScan();
		CoLaBMessage response;
		auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
		if (ec) {
			return ec;
		}

		ec = impl_->validate_response(response, CommandType::kReadAnswer, "LMDscandata");
		if (ec) {
			return ec;
		}

		return ScanDataParser::Parse(response.payload.data(), response.payload.size(), out);
	}


	std::error_code LMS4xxxDriver::StartMeasurement() {
		// Requires login first. Caller should call configure() which includes login.
		auto frame = CommandBuilder::BuildStartMeasurement();
		CoLaBMessage response;
		auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
		if (ec) {
			return ec;
		}

		ec = impl_->validate_response(response, CommandType::kMethodAnswer, "LMCstartmeas");
		if (ec) {
			return ec;
		}

		if (response.payload.empty() || response.payload[0] != 0x00) {
			return make_error_code(ErrorCode::kCommandRejected);
		}
		g_log.trace("Measurement started");
		return {};
	}


	std::error_code LMS4xxxDriver::StopMeasurement() {
		auto frame = CommandBuilder::BuildStopMeasurement();
		CoLaBMessage response;
		auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
		if (ec) {
			return ec;
		}

		ec = impl_->validate_response(response, CommandType::kMethodAnswer, "LMCstopmeas");
		if (ec) {
			return ec;
		}

		if (response.payload.empty() || response.payload[0] != 0x00) {
			return make_error_code(ErrorCode::kCommandRejected);
		}
		g_log.trace("Measurement stopped");
		return {};
	}


	std::error_code LMS4xxxDriver::Standby() {
		auto frame = CommandBuilder::BuildStandby();
		CoLaBMessage response;
		auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
		if (ec) {
			return ec;
		}

		ec = impl_->validate_response(response, CommandType::kMethodAnswer, "LMCstandby");
		if (ec) {
			return ec;
		}

		if (response.payload.empty() || response.payload[0] != 0x00) {
			return make_error_code(ErrorCode::kCommandRejected);
		}
		g_log.info("Entered standby mode");
		return {};
	}


	std::error_code LMS4xxxDriver::RebootDevice() {
		auto frame = CommandBuilder::BuildReboot();
		CoLaBMessage response;
		auto ec = impl_->send_and_receive(frame, response, impl_->config.network.response_timeout_ms);
		if (ec) {
			return ec;
		}

		g_log.info("Reboot command sent");
		return {};
	}


	void LMS4xxxDriver::SetScanConfig(const ScanConfig &config) {
		impl_->config.scan = config;
	}

	const DriverConfig &LMS4xxxDriver::GetConfig() const {
		return impl_->config;
	}


	std::error_code DriverConfig::Validate() const {
		if (device.ip.empty()) {
			g_log.error("Device IP is empty");
			return make_error_code(ErrorCode::kInvalidConfig);
		}
		if (device.port == 0) {
			g_log.error("Device port is 0");
			return make_error_code(ErrorCode::kInvalidConfig);
		}
		if (scan.start_angle_deg >= scan.stop_angle_deg) {
			g_log.error("Start angle ({}) must be less than stop angle ({})", scan.start_angle_deg, scan.stop_angle_deg);
			return make_error_code(ErrorCode::kInvalidConfig);
		}
		if (scan.angular_resolution_deg <= 0.0) {
			g_log.error("Angular resolution must be positive");
			return make_error_code(ErrorCode::kInvalidConfig);
		}
		if (scan.output_rate == 0) {
			g_log.error("Output rate must be >= 1");
			return make_error_code(ErrorCode::kInvalidConfig);
		}
		if (network.ring_buffer_frames < 16) {
			g_log.error("Ring buffer too small: {} (minimum 16)", network.ring_buffer_frames);
			return make_error_code(ErrorCode::kInvalidConfig);
		}
		if (ntp.enable && ntp.server_ip.empty()) {
			g_log.error("NTP enabled but server IP is empty");
			return make_error_code(ErrorCode::kInvalidConfig);
		}
		return {};
	}


}  // namespace LMS4xxx
