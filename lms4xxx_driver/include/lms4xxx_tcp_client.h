#ifndef LMS4XXX_TCP_CLIENT_H
#define LMS4XXX_TCP_CLIENT_H

#include "lms4xxx_config.h"
#include "tcp_client.h"


namespace LMS4xxx {

	// The TCP client implementation lives in common (Common::TcpClient); this
	// adapter maps the LMS4xxx config structs onto its options.
	using TCPClient = Common::TcpClient;

	[[nodiscard]] inline Common::TcpClient::Options MakeTcpOptions(const DeviceConfig &device, const NetworkConfig &network) {
		Common::TcpClient::Options opts;
		opts.host = device.ip;
		opts.port = device.port;
		opts.recv_buffer_bytes = network.recv_buffer_bytes;
		opts.tcp_keepalive = network.tcp_keepalive;
		opts.keepalive_idle_s = network.keepalive_idle_s;
		opts.keepalive_interval_s = network.keepalive_interval_s;
		opts.keepalive_count = network.keepalive_count;
		return opts;
	}

}  // namespace LMS4xxx

#endif	// LMS4XXX_TCP_CLIENT_H
