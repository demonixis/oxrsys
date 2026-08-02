// SPDX-License-Identifier: MPL-2.0

#include "EncoderIpcSocket.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace oxrsys::encoder::ipc
{

EncoderIpcSocket::~EncoderIpcSocket()
{
    Close();
}

void EncoderIpcSocket::Adopt(int fd)
{
    Close();
    fd_ = fd;
}

void EncoderIpcSocket::ShutdownBoth()
{
    if (fd_ >= 0)
    {
        ::shutdown(fd_, SHUT_RDWR);
    }
}

void EncoderIpcSocket::Close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

IoResult EncoderIpcSocket::ReadExact(void* buffer, size_t size)
{
    uint8_t* p = static_cast<uint8_t*>(buffer);
    size_t got = 0;
    while (got < size)
    {
        const ssize_t n = ::read(fd_, p + got, size - got);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            // A peer death can surface as ECONNRESET instead of EOF; both mean
            // "the helper is gone" to the caller.
            if (errno == ECONNRESET)
            {
                return IoResult::Eof;
            }
            return IoResult::Error;
        }
        if (n == 0)
        {
            // EOF mid-message is still EOF: the caller's crash detection owns
            // the distinction between clean close and truncation.
            return IoResult::Eof;
        }
        got += (size_t)n;
    }
    return IoResult::Ok;
}

IoResult EncoderIpcSocket::ReadMessage(MessageHeader& header, std::vector<uint8_t>& payload)
{
    if (fd_ < 0)
    {
        return IoResult::Error;
    }
    uint8_t headerBytes[MessageHeader::kWireSize];
    const IoResult headerResult = ReadExact(headerBytes, sizeof(headerBytes));
    if (headerResult != IoResult::Ok)
    {
        return headerResult;
    }
    if (!MessageHeader::Deserialize(headerBytes, sizeof(headerBytes), header) ||
        !ValidateHeader(header))
    {
        return IoResult::Invalid;
    }
    payload.clear();
    if (header.payloadSize == 0)
    {
        return IoResult::Ok;
    }
    // Bounded by ValidateHeader (<= kMaxPayloadSize) before this allocation.
    payload.resize(header.payloadSize);
    return ReadExact(payload.data(), payload.size());
}

IoResult EncoderIpcSocket::WriteMessage(MessageType type, uint16_t flags, uint64_t sequence,
                                        const uint8_t* payload, size_t payloadSize)
{
    if (fd_ < 0)
    {
        return IoResult::Error;
    }
    if (payloadSize > kMaxPayloadSize)
    {
        return IoResult::Invalid;
    }
    MessageHeader header;
    header.type = (uint16_t)type;
    header.flags = flags;
    header.payloadSize = (uint32_t)payloadSize;
    header.sequence = sequence;
    uint8_t headerBytes[MessageHeader::kWireSize];
    header.Serialize(headerBytes);

    struct iovec iov[2];
    iov[0].iov_base = headerBytes;
    iov[0].iov_len = sizeof(headerBytes);
    iov[1].iov_base = const_cast<uint8_t*>(payload);
    iov[1].iov_len = payloadSize;
    int iovCount = payloadSize > 0 ? 2 : 1;

    size_t skip = 0; // bytes already written across the whole message
    const size_t total = sizeof(headerBytes) + payloadSize;
    while (skip < total)
    {
        // Rebuild the iov view for the unwritten remainder.
        struct iovec view[2];
        int viewCount = 0;
        size_t consumed = 0;
        for (int i = 0; i < iovCount; i++)
        {
            const size_t end = consumed + iov[i].iov_len;
            if (skip < end)
            {
                const size_t within = skip > consumed ? skip - consumed : 0;
                view[viewCount].iov_base = (uint8_t*)iov[i].iov_base + within;
                view[viewCount].iov_len = iov[i].iov_len - within;
                viewCount++;
            }
            consumed = end;
        }
        const ssize_t n = ::writev(fd_, view, viewCount);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET)
            {
                return IoResult::Eof;
            }
            return IoResult::Error;
        }
        skip += (size_t)n;
    }
    return IoResult::Ok;
}

void EncoderIpcSocket::SetNoSigPipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

void EncoderIpcSocket::SetCloseOnExec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
    {
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

} // namespace oxrsys::encoder::ipc
