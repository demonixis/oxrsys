// SPDX-License-Identifier: MPL-2.0

#include "RuntimeSockets.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace oxrsys::runtime_socket
{

namespace
{

int ClampSocketSize(size_t size)
{
    return static_cast<int>(
        std::min(size, static_cast<size_t>(std::numeric_limits<int>::max())));
}

bool SetIntOption(SocketHandle socket, int level, int option, int value)
{
    const void* optionValue = &value;
    return setsockopt(socket, level, option, optionValue, sizeof(value)) == 0;
}

} // namespace

bool IsValid(SocketHandle socket)
{
    return socket != InvalidSocket;
}

bool IsInterruptedOrWouldBlock()
{
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

std::string LastErrorText()
{
    return std::strerror(errno);
}

SocketHandle Create(int domain, int type, int protocol)
{
    return socket(domain, type, protocol);
}

void Close(SocketHandle& socket)
{
    if (!IsValid(socket))
    {
        return;
    }
    close(socket);
    socket = InvalidSocket;
}

void Shutdown(SocketHandle socket)
{
    if (!IsValid(socket))
    {
        return;
    }
    shutdown(socket, SHUT_RDWR);
}

void ShutdownAndClose(SocketHandle& socket)
{
    if (!IsValid(socket))
    {
        return;
    }
    Shutdown(socket);
    Close(socket);
}

bool SetReuseAddress(SocketHandle socket)
{
    return IsValid(socket) && SetIntOption(socket, SOL_SOCKET, SO_REUSEADDR, 1);
}

bool SetBroadcast(SocketHandle socket)
{
    return IsValid(socket) && SetIntOption(socket, SOL_SOCKET, SO_BROADCAST, 1);
}

bool SetSendBuffer(SocketHandle socket, int bytes)
{
    return IsValid(socket) && SetIntOption(socket, SOL_SOCKET, SO_SNDBUF, bytes);
}

bool SetReceiveTimeout(SocketHandle socket, long seconds, long microseconds)
{
    if (!IsValid(socket))
    {
        return false;
    }
    timeval timeout = {};
    timeout.tv_sec = seconds;
    timeout.tv_usec = microseconds;
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
}

bool SetSendTimeout(SocketHandle socket, long seconds, long microseconds)
{
    if (!IsValid(socket))
    {
        return false;
    }
    timeval timeout = {};
    timeout.tv_sec = seconds;
    timeout.tv_usec = microseconds;
    return setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

bool SetTcpNoDelay(SocketHandle socket)
{
    return IsValid(socket) && SetIntOption(socket, IPPROTO_TCP, TCP_NODELAY, 1);
}

bool SetNoSigpipe(SocketHandle socket)
{
#if defined(SO_NOSIGPIPE)
    return IsValid(socket) && SetIntOption(socket, SOL_SOCKET, SO_NOSIGPIPE, 1);
#else
    (void)socket;
    return true;
#endif
}

bool SetNonBlocking(SocketHandle socket, bool enabled)
{
    if (!IsValid(socket))
    {
        return false;
    }
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    const int nextFlags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(socket, F_SETFL, nextFlags) == 0;
}

int SelectOneReadable(SocketHandle socket, long seconds, long microseconds)
{
    if (!IsValid(socket))
    {
        return -1;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout = {};
    timeout.tv_sec = seconds;
    timeout.tv_usec = microseconds;

    return select(socket + 1, &readSet, nullptr, nullptr, &timeout);
}

int Send(SocketHandle socket, const void* data, size_t size, int flags)
{
    if (!IsValid(socket))
    {
        return -1;
    }
    const int chunkSize = ClampSocketSize(size);
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif
    return static_cast<int>(send(socket, data, static_cast<size_t>(chunkSize), flags));
}

int Receive(SocketHandle socket, void* data, size_t size, int flags)
{
    if (!IsValid(socket))
    {
        return -1;
    }
    const int chunkSize = ClampSocketSize(size);
    return static_cast<int>(recv(socket, data, static_cast<size_t>(chunkSize), flags));
}

int SendTo(SocketHandle socket, const void* data, size_t size, int flags,
           const sockaddr* address, SocketLength addressLength)
{
    if (!IsValid(socket))
    {
        return -1;
    }
    const int chunkSize = ClampSocketSize(size);
    return static_cast<int>(sendto(socket,
                                   data,
                                   static_cast<size_t>(chunkSize),
                                   flags,
                                   address,
                                   addressLength));
}

int ReceiveFrom(SocketHandle socket, void* data, size_t size, int flags,
                sockaddr* address, SocketLength* addressLength)
{
    if (!IsValid(socket))
    {
        return -1;
    }
    const int chunkSize = ClampSocketSize(size);
    return static_cast<int>(recvfrom(socket,
                                     data,
                                     static_cast<size_t>(chunkSize),
                                     flags,
                                     address,
                                     addressLength));
}

} // namespace oxrsys::runtime_socket
