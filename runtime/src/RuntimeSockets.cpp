// SPDX-License-Identifier: MPL-2.0

#include "RuntimeSockets.h"

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

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
#if defined(_WIN32)
    const char* optionValue = reinterpret_cast<const char*>(&value);
#else
    const void* optionValue = &value;
#endif
    return setsockopt(socket, level, option, optionValue, sizeof(value)) == 0;
}

} // namespace

bool EnsureInitialized()
{
#if defined(_WIN32)
    static bool initialized = [] {
        WSADATA data = {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
#else
    return true;
#endif
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

std::string LastErrorText()
{
#if defined(_WIN32)
    const int error = WSAGetLastError();
    char* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(flags,
                                        nullptr,
                                        static_cast<DWORD>(error),
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPSTR>(&message),
                                        0,
                                        nullptr);
    if (length == 0 || message == nullptr)
    {
        return "Winsock error " + std::to_string(error);
    }
    std::string result(message, length);
    LocalFree(message);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
    {
        result.pop_back();
    }
    return result;
#else
    return std::strerror(errno);
#endif
}

SocketHandle Create(int domain, int type, int protocol)
{
    if (!EnsureInitialized())
    {
        return InvalidSocket;
    }
    return socket(domain, type, protocol);
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
#if defined(_WIN32)
    DWORD timeoutMs = static_cast<DWORD>(seconds * 1000 + microseconds / 1000);
    return setsockopt(socket,
                      SOL_SOCKET,
                      SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&timeoutMs),
                      sizeof(timeoutMs)) == 0;
#else
    timeval timeout = {};
    timeout.tv_sec = seconds;
    timeout.tv_usec = microseconds;
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
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
#if defined(_WIN32)
    u_long mode = enabled ? 1u : 0u;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    const int nextFlags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(socket, F_SETFL, nextFlags) == 0;
#endif
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

#if defined(_WIN32)
    return select(0, &readSet, nullptr, nullptr, &timeout);
#else
    return select(socket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
}

int Send(SocketHandle socket, const void* data, size_t size, int flags)
{
    if (!IsValid(socket))
    {
        return -1;
    }
    const int chunkSize = ClampSocketSize(size);
#if defined(_WIN32)
    return send(socket, static_cast<const char*>(data), chunkSize, flags);
#else
    return static_cast<int>(send(socket, data, static_cast<size_t>(chunkSize), flags));
#endif
}

int Receive(SocketHandle socket, void* data, size_t size, int flags)
{
    if (!IsValid(socket))
    {
        return -1;
    }
    const int chunkSize = ClampSocketSize(size);
#if defined(_WIN32)
    return recv(socket, static_cast<char*>(data), chunkSize, flags);
#else
    return static_cast<int>(recv(socket, data, static_cast<size_t>(chunkSize), flags));
#endif
}

int SendTo(SocketHandle socket, const void* data, size_t size, int flags,
           const sockaddr* address, SocketLength addressLength)
{
    if (!IsValid(socket))
    {
        return -1;
    }
    const int chunkSize = ClampSocketSize(size);
#if defined(_WIN32)
    return sendto(socket,
                  static_cast<const char*>(data),
                  chunkSize,
                  flags,
                  address,
                  addressLength);
#else
    return static_cast<int>(sendto(socket,
                                   data,
                                   static_cast<size_t>(chunkSize),
                                   flags,
                                   address,
                                   addressLength));
#endif
}

int ReceiveFrom(SocketHandle socket, void* data, size_t size, int flags,
                sockaddr* address, SocketLength* addressLength)
{
    if (!IsValid(socket))
    {
        return -1;
    }
    const int chunkSize = ClampSocketSize(size);
#if defined(_WIN32)
    return recvfrom(socket, static_cast<char*>(data), chunkSize, flags, address, addressLength);
#else
    return static_cast<int>(recvfrom(socket,
                                     data,
                                     static_cast<size_t>(chunkSize),
                                     flags,
                                     address,
                                     addressLength));
#endif
}

} // namespace oxrsys::runtime_socket
