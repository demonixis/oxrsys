// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "EncoderIpcProtocol.h"

/**
 * Framed message I/O over a Unix stream socket fd for the encoder helper
 * protocol (EncoderIpcProtocol.h).
 *
 * - ReadMessage: header first (bounded by ValidateHeader), then exactly
 *   payloadSize bytes. EINTR and partial reads handled; EOF is a distinct
 *   result so the caller can treat helper death as a state, not an error.
 * - WriteMessage: header + payload in one writev, with EINTR/partial-write
 *   continuation.
 * - No thread-safety is provided here: callers serialize writes through a
 *   single writer thread (blocking writes must never run on a time-critical
 *   thread — a full socket buffer would stall the Metal completed handler).
 */
namespace oxrsys::encoder::ipc
{

enum class IoResult
{
    Ok,
    Eof,      ///< orderly peer close (or reset surfaced as EOF)
    Error,    ///< errno-style failure
    Invalid,  ///< protocol violation (bad magic/version/oversize)
};

class EncoderIpcSocket
{
public:
    EncoderIpcSocket() = default;
    explicit EncoderIpcSocket(int fd) : fd_(fd) {}
    ~EncoderIpcSocket();

    EncoderIpcSocket(const EncoderIpcSocket&) = delete;
    EncoderIpcSocket& operator=(const EncoderIpcSocket&) = delete;

    void Adopt(int fd);
    int Fd() const { return fd_; }
    bool IsOpen() const { return fd_ >= 0; }
    /// shutdown(SHUT_RDWR) without closing: unblocks a reader on another thread.
    void ShutdownBoth();
    void Close();

    /**
     * Read one framed message. On Ok, `header` passed ValidateHeader and
     * `payload` holds exactly header.payloadSize bytes.
     */
    IoResult ReadMessage(MessageHeader& header, std::vector<uint8_t>& payload);

    /// Write one framed message (header fields filled from the arguments).
    IoResult WriteMessage(MessageType type, uint16_t flags, uint64_t sequence,
                          const uint8_t* payload, size_t payloadSize);
    IoResult WriteMessage(MessageType type, uint16_t flags, uint64_t sequence,
                          const std::vector<uint8_t>& payload)
    {
        return WriteMessage(type, flags, sequence, payload.data(), payload.size());
    }

    /// Suppress SIGPIPE on this socket (parent must never die to a helper crash).
    static void SetNoSigPipe(int fd);
    static void SetCloseOnExec(int fd);

private:
    IoResult ReadExact(void* buffer, size_t size);

    int fd_ = -1;
};

} // namespace oxrsys::encoder::ipc
