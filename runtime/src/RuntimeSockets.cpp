// SPDX-License-Identifier: MPL-2.0

#include "RuntimeSockets.h"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
#include <cstdio>
#else
#include <cerrno>
#include <netinet/tcp.h>
#include <unistd.h>
#if defined(SO_NOSIGPIPE)
#include <sys/socket.h>
#endif
#include <fcntl.h>
#endif

namespace oxrsys::runtime_socket
{

bool EnsureInitialized()
{
#if defined(_WIN32)
    static std::atomic<bool> initialized{false};
    bool expected = false;
    if (initialized.compare_exchange_strong(expected, true))
    {
        WSADATA data = {};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            initialized.store(false);
            return false;
        }
    }
#endif
    return true;
}

bool IsValid(SocketHandle socket)
{
    return socket != InvalidSocket;
}

bool IsInterruptedOrWouldBlock()
{
#if defined(_WIN32)
    const int error = WSAGetLastError();
    return error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
#else
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void Close(SocketHandle& socket)
{
    if (!IsValid(socket))
    {
        return;
    }
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
    socket = InvalidSocket;
}

void ShutdownAndClose(SocketHandle& socket)
{
    if (!IsValid(socket))
    {
        return;
    }
#if defined(_WIN32)
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
    Close(socket);
}

SocketHandle Create(int addressFamily, int type, int protocol)
{
    if (!EnsureInitialized())
    {
        return InvalidSocket;
    }
    return ::socket(addressFamily, type, protocol);
}

bool SetReuseAddress(SocketHandle socket)
{
    int yes = 1;
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&yes), sizeof(yes)) == 0;
}

bool SetBroadcast(SocketHandle socket)
{
    int yes = 1;
    return setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
                      reinterpret_cast<const char*>(&yes), sizeof(yes)) == 0;
}

bool SetSendBuffer(SocketHandle socket, int bytes)
{
    return setsockopt(socket, SOL_SOCKET, SO_SNDBUF,
                      reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool SetReceiveTimeout(SocketHandle socket, int seconds, int microseconds)
{
#if defined(_WIN32)
    DWORD timeoutMs = static_cast<DWORD>(seconds * 1000 + microseconds / 1000);
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == 0;
#else
    timeval timeout = {seconds, microseconds};
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

bool SetTcpNoDelay(SocketHandle socket)
{
    int yes = 1;
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&yes), sizeof(yes)) == 0;
}

bool SetNoSigpipe(SocketHandle socket)
{
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
    int yes = 1;
    return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0;
#else
    (void)socket;
    return true;
#endif
}

bool SetNonBlocking(SocketHandle socket, bool enabled)
{
#if defined(_WIN32)
    u_long value = enabled ? 1 : 0;
    return ioctlsocket(socket, FIONBIO, &value) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    if (enabled)
    {
        flags |= O_NONBLOCK;
    }
    else
    {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(socket, F_SETFL, flags) == 0;
#endif
}

int SelectOneReadable(SocketHandle socket, int timeoutSeconds, int timeoutMicroseconds)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout = {timeoutSeconds, timeoutMicroseconds};
#if defined(_WIN32)
    return select(0, &readSet, nullptr, nullptr, &timeout);
#else
    return select(socket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
}

int64_t Send(SocketHandle socket, const void* data, size_t size, int flags)
{
#if defined(_WIN32)
    return static_cast<int64_t>(send(socket, static_cast<const char*>(data),
                                     static_cast<int>(size), flags));
#else
    return static_cast<int64_t>(send(socket, data, size, flags));
#endif
}

int64_t Receive(SocketHandle socket, void* data, size_t size, int flags)
{
#if defined(_WIN32)
    return static_cast<int64_t>(recv(socket, static_cast<char*>(data),
                                     static_cast<int>(size), flags));
#else
    return static_cast<int64_t>(recv(socket, data, size, flags));
#endif
}

int64_t SendTo(SocketHandle socket, const void* data, size_t size, int flags,
               const sockaddr* address, SockLen addressLength)
{
#if defined(_WIN32)
    return static_cast<int64_t>(sendto(socket, static_cast<const char*>(data),
                                       static_cast<int>(size), flags, address, addressLength));
#else
    return static_cast<int64_t>(sendto(socket, data, size, flags, address, addressLength));
#endif
}

int64_t ReceiveFrom(SocketHandle socket, void* data, size_t size, int flags,
                    sockaddr* address, SockLen* addressLength)
{
#if defined(_WIN32)
    return static_cast<int64_t>(recvfrom(socket, static_cast<char*>(data),
                                         static_cast<int>(size), flags, address, addressLength));
#else
    return static_cast<int64_t>(recvfrom(socket, data, size, flags, address, addressLength));
#endif
}

const char* LastErrorText()
{
#if defined(_WIN32)
    static thread_local char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "WSA error %d", WSAGetLastError());
    return buffer;
#else
    return std::strerror(errno);
#endif
}

} // namespace oxrsys::runtime_socket
