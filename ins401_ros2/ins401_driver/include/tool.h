#pragma once

#include <vector>
#include <iomanip>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <map>
#include <sstream>
#include <algorithm>
#include <cerrno>


#include "data_type.h"


namespace Tool {
	namespace Ethernet {
		std::vector<std::pair<std::string, std::string> > GetNetworkInterfaces();

		std::string FormatMacAddress(const uint8_t *mac);

		void ConvertUint16ToUint8(const uint16_t &uint16, uint8_t* uint8, ENDIAN_TYPE type);

		void ParseMACAddressToUint8(const std::string &mac_str, uint8_t* mac_uint8);
	}


	namespace INS401 {
		uint16_t CalcCRC(const uint8_t *buf, const uint16_t &length);
	}
}
