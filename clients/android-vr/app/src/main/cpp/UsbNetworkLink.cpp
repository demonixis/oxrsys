// SPDX-License-Identifier: MPL-2.0

#include "UsbNetworkLink.h"

#include <android/log.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <jni.h>

#define LOG_TAG "OXRSys-UsbNetwork"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace oxr::usb_network
{

namespace
{
std::atomic<net_handle_t> g_network{NETWORK_UNSPECIFIED};
}

net_handle_t CurrentNetwork()
{
    return g_network.load();
}

void SetCurrentNetwork(net_handle_t network)
{
    if (g_network.exchange(network) != network)
    {
        LOGI("USB network %s (handle %llu)",
             network != NETWORK_UNSPECIFIED ? "available" : "lost",
             static_cast<unsigned long long>(network));
    }
}

bool IsServerAddress(const char* ip)
{
    return ip != nullptr && strcmp(ip, kServerAddress) == 0;
}

bool BindSocketForServer(int socket, const char* ip)
{
    if (!IsServerAddress(ip))
    {
        return true;
    }
    const net_handle_t network = CurrentNetwork();
    if (network == NETWORK_UNSPECIFIED || android_setsocknetwork(network, socket) != 0)
    {
        LOGE("Failed to bind socket to the USB network (handle %llu): %s",
             static_cast<unsigned long long>(network), strerror(errno));
        return false;
    }
    return true;
}

} // namespace oxr::usb_network

extern "C" JNIEXPORT void JNICALL
Java_com_oculus_NativeActivity_nativeSetUsbNetwork(JNIEnv*, jclass, jlong networkHandle)
{
    oxr::usb_network::SetCurrentNetwork(static_cast<net_handle_t>(networkHandle));
}
