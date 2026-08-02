// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>

/**
 * The native-arm64 out-of-process encoder helper's server loop.
 *
 * Owns the child side of the encoder IPC contract:
 *  - Unix-socket protocol (EncoderIpcProtocol.h) adopted from --socket-fd.
 *  - Mach surface rendezvous (EncoderMachSurface.h) under --bootstrap-name.
 *  - Capability probe by TRIAL SESSION CREATE (never VTCopyVideoEncoderList;
 *    on arm64 the UsingHardwareAcceleratedVideoEncoder query itself fails
 *    -12900 — RequireHardware create success is the proof of hardware).
 *  - Registered IOSurfaces are wrapped ONCE per generation via
 *    CVPixelBufferCreateWithIOSurface with re-applied BT.709
 *    ShouldPropagate attachments, then reused for every frame (Gate B2).
 *  - Frames drive the shared VideoToolboxEncodeEngine (the same engine the
 *    in-process transport uses — no duplicated VideoToolbox code).
 *  - Encoded output flows through a bounded result-writer queue to the
 *    socket; VT callbacks never block on socket backpressure.
 *  - Socket EOF is an immediate clean exit: the parent is gone.
 *
 * The helper links only Foundation/CoreFoundation/CoreMedia/CoreVideo/
 * IOSurface/Metal/VideoToolbox (+ the header-only/utility C++ deps of the
 * engine). It must never reference ALVR symbols.
 */
namespace oxrsys::encoder::helper
{

class EncoderHelperServer
{
public:
    /// Runs to completion; the return value is the process exit code.
    int Run(int socketFd, const std::string& bootstrapName);
};

} // namespace oxrsys::encoder::helper
