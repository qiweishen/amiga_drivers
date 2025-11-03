#include "ins_device_finder.h"
#include "ins_driver.h"


int main() {
    INSDeviceDiscovery discovery;
    std::map<std::string, DeviceInfo> devices = discovery.GetDiscoveredDevices();
    for (const auto device: devices) {
        std::cout << "Device model: " << device.second.product << std::endl;
        std::cout << "Interface: " << device.first << std::endl;
        std::cout << "Mac address: " << device.second.mac_address << std::endl;
    }

    const std::string interface_name = "enp49s0";
    const std::string imu_mac = "a1:52:8c:95:00:28";

    std::cout << "================================" << std::endl;
    std::cout << "Interface: " << interface_name << std::endl;
    std::cout << "Target MAC: " << imu_mac << std::endl;
    std::cout << std::endl;

    std::unique_ptr<EthernetINSReceiver> receiverPtr = std::make_unique<EthernetINSReceiver>(
        interface_name, imu_mac, true);
    std::thread receiver_thread([&receiverPtr]() {
        receiverPtr->Run();
    });

    while (true) {
        continue;
    }

    return 0;
}
