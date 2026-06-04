// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace oxrsys::runtime_socket
{

#if defined(_WIN32)
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle InvalidSocket = -1;
#endif

bool EnsureInitialized();
bool IsValid(SocketHandle socket);
bool IsInterruptedOrWouldBlock();
std::string LastErrorText();

SocketHandle Create(int domain, int type, int protocol);
void Close(SocketHandle& socket);
void ShutdownAndClose(SocketHandle& socket);

bool SetReuseAddress(SocketHandle socket);
bool SetBroadcast(SocketHandle socket);
bool SetSendBuffer(SocketHandle socket, int bytes);
bool SetReceiveTimeout(SocketHandle socket, long seconds, long microseconds);
bool SetTcpNoDelay(SocketHandle socket);
bool SetNoSigpipe(SocketHandle socket);
bool SetNonBlocking(SocketHandle socket, bool enabled);

int SelectOneReadable(SocketHandle socket, long seconds, long microseconds);
int Send(SocketHandle socket, const void* data, size_t size, int flags);
int Receive(SocketHandle socket, void* data, size_t size, int flags);
int SendTo(SocketHandle socket, const void* data, size_t size, int flags,
           const sockaddr* address, SocketLength addressLength);
int ReceiveFrom(SocketHandle socket, void* data, size_t size, int flags,
                sockaddr* address, SocketLength* addressLength);

} // namespace oxrsys::runtime_socket
