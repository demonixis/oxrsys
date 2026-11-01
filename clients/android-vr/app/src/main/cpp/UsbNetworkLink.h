// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <android/multinetwork.h>

namespace oxr::usb_network
{

// OXRSys Home switches the Quest's USB function to CDC-NCM and gives the cable link fixed
// addresses: the Mac is 192.168.42.1, the Quest 192.168.42.2. The Quest has no gateway on it, so
// its default network stays Wi-Fi and sockets to the Mac must be bound to the Ethernet network.
constexpr const char* kServerAddress = "192.168.42.1";

// Ethernet network handle reported by NativeActivity, or NETWORK_UNSPECIFIED when absent.
net_handle_t CurrentNetwork();
void SetCurrentNetwork(net_handle_t network);

bool IsServerAddress(const char* ip);

// Binds the socket to the cable network when ip is the link's Mac address. Returns false only
// when that bind was required and failed.
bool BindSocketForServer(int socket, const char* ip);

} // namespace oxr::usb_network
