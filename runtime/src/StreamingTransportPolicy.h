// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <arpa/inet.h>
#include <cstdint>
#include <string>

namespace oxrsys::streaming_transport
{

inline bool UsbAdbTcpListenersReady(bool controlReady,
                                    bool videoReady,
                                    bool trackingReady,
                                    bool spatialReady,
                                    bool spatialBackendAttached)
{
    return controlReady &&
           videoReady &&
           trackingReady &&
           (!spatialBackendAttached || spatialReady);
}

// OXRSys Home gives the USB network (CDC-NCM) link fixed addresses in 192.168.42.0/24: the Mac
// is 192.168.42.1 and the Quest 192.168.42.2. Clients on it use the WiFi UDP path.
inline bool IsUsbNetworkClientAddress(const std::string& ip)
{
    in_addr addr = {};
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1)
    {
        return false;
    }
    return (ntohl(addr.s_addr) & 0xFFFFFF00u) == 0xC0A82A00u;
}

} // namespace oxrsys::streaming_transport
