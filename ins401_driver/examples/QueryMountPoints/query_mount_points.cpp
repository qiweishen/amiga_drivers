#include <INIReader.h>

#include "ntrip_client.h"


int main(int argc, char *argv[]) {
    std::string config_path = argc > 1 ? argv[1]
                                       : std::string(PROJECT_SOURCE_DIR) + "/Config.ini";
    const INIReader configures(config_path);
    // Configure NTRIP client.
    auto ntrip_client_ptr = std::make_unique<NTRIPClient>(configures, "./");
    std::vector<NTRIPClient::MountPoint> mount_points = ntrip_client_ptr->GetSourceTable();

    return 0;
}
