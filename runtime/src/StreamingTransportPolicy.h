// SPDX-License-Identifier: MPL-2.0

#pragma once

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

} // namespace oxrsys::streaming_transport
