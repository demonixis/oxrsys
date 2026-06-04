// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <cstddef>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace oxrsys::runtime_socket
{

#if defined(_WIN32)
using SocketHandle = SOCKET;
using SockLen = int;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
using SockLen = socklen_t;
constexpr SocketHandle InvalidSocket = -1;
#endif

bool EnsureInitialized();
bool IsValid(SocketHandle socket);
bool IsInterruptedOrWouldBlock();
void Close(SocketHandle& socket);
void ShutdownAndClose(SocketHandle& socket);

SocketHandle Create(int addressFamily, int type, int protocol);
bool SetReuseAddress(SocketHandle socket);
bool SetBroadcast(SocketHandle socket);
bool SetSendBuffer(SocketHandle socket, int bytes);
bool SetReceiveTimeout(SocketHandle socket, int seconds, int microseconds);
bool SetTcpNoDelay(SocketHandle socket);
bool SetNoSigpipe(SocketHandle socket);
bool SetNonBlocking(SocketHandle socket, bool enabled);

int SelectOneReadable(SocketHandle socket, int timeoutSeconds, int timeoutMicroseconds);
int64_t Send(SocketHandle socket, const void* data, size_t size, int flags);
int64_t Receive(SocketHandle socket, void* data, size_t size, int flags);
int64_t SendTo(SocketHandle socket, const void* data, size_t size, int flags,
               const sockaddr* address, SockLen addressLength);
int64_t ReceiveFrom(SocketHandle socket, void* data, size_t size, int flags,
                    sockaddr* address, SockLen* addressLength);

const char* LastErrorText();

} // namespace oxrsys::runtime_socket
