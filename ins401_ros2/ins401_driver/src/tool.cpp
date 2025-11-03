#include "tool.h"


namespace Tool {
    namespace Ethernet {
        std::vector<std::pair<std::string, std::string> > GetNetworkInterfaces() {
            std::vector<std::pair<std::string, std::string> > interfaces;
            ifaddrs *ifaddr;
            if (getifaddrs(&ifaddr) == -1) {
                std::cerr << "Error: Failed to get network interfaces - " << strerror(errno) << std::endl;
                return interfaces;
            }
            for (ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
                if (ifa->ifa_addr == nullptr) {
                    continue;
                }
                if (ifa->ifa_addr->sa_family == AF_PACKET) {
                    if (const auto *s = reinterpret_cast<struct sockaddr_ll *>(ifa->ifa_addr);
                        s->sll_halen == 6 && strcmp(ifa->ifa_name, "lo") != 0) {
                        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
                        ifreq ifr{};
                        memset(&ifr, 0, sizeof(ifr));
                        strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);

                        bool is_up = false;
                        if (ioctl(fd, SIOCGIFFLAGS, &ifr) >= 0) {
                            is_up = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
                        }
                        close(fd);
                        if (is_up) {
                            std::stringstream mac_stream;
                            for (int i = 0; i < 6; i++) {
                                if (i > 0) mac_stream << ":";
                                mac_stream << std::hex << std::setw(2) << std::setfill('0') << (int) s->sll_addr[i];
                            }
                            interfaces.emplace_back(ifa->ifa_name, mac_stream.str());
                        }
                    }
                }
            }
            freeifaddrs(ifaddr);
            return interfaces;
        }


        std::string FormatMacAddress(const uint8_t *mac) {
            std::stringstream ss;
            for (int i = 0; i < 6; i++) {
                if (i > 0) ss << ":";
                ss << std::hex << std::setw(2) << std::setfill('0') << (int) mac[i];
            }
            return ss.str();
        }


        void ConvertUint16ToUint8(const uint16_t &uint16, uint8_t *uint8, ENDIAN_TYPE type) {
            if (type == LSB) {
                // LSB-first
                uint8[0] = static_cast<uint8_t>(uint16 & 0x00FF);
                uint8[1] = static_cast<uint8_t>((uint16 >> 8) & 0x00FF);
            } else if (type == MSB) {
                // MSB-first
                uint8[0] = static_cast<uint8_t>((uint16 >> 8) & 0x00FF);
                uint8[1] = static_cast<uint8_t>(uint16 & 0x00FF);
            } else {
                std::cerr << "Invalid ENDIAN_TYPE specified." << std::endl;
            }
        }


        void ParseMACAddressToUint8(const std::string &mac_str, uint8_t *mac_uint8) {
            if (mac_str.empty() || mac_uint8 == nullptr) {
                std::cerr << "Empty MAC address string or null output array." << std::endl;
            }
            const int result = std::sscanf(mac_str.c_str(),
                                           "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                                           &mac_uint8[0], &mac_uint8[1], &mac_uint8[2],
                                           &mac_uint8[3], &mac_uint8[4], &mac_uint8[5]);
            if (result != 6) {
                std::cerr << "Invalid MAC address format: " << mac_str << std::endl;
            }
        }
    }


    namespace INS401 {
        uint16_t CalcCRC(const uint8_t *buf, const uint16_t &length) {
            uint16_t crc = 0x1D0F; // CRC-16/AUG-CCITT, poly=0x1021, refin/out=false
            for (uint16_t i = 0; i < length; i++) {
                crc ^= static_cast<uint16_t>(buf[i]) << 8;
                for (int j = 0; j < 8; j++) {
                    if (crc & 0x8000) {
                        crc = static_cast<uint16_t>(((crc << 1) ^ 0x1021) & 0xFFFF);
                    } else {
                        crc = static_cast<uint16_t>((crc << 1) & 0xFFFF);
                    }
                }
            }
            return crc;
        }
    }
}
