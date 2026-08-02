// SPDX-License-Identifier: MPL-2.0

// oxrsys-encoder-helper: the native-arm64 out-of-process encoder.
//
// Spawned by NativeHelperEncoderTransport with:
//   oxrsys-encoder-helper --socket-fd <N> --bootstrap-name <name>
//
// The socket fd (a socketpair end inherited by number) carries the framed
// control/frame protocol (EncoderIpcProtocol.h); the bootstrap name is the
// parent's pre-checked-in Mach rendezvous for IOSurface transfer
// (EncoderMachSurface.h). Everything else lives in EncoderHelperServer.

#import <Foundation/Foundation.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "EncoderHelperServer.h"

int main(int argc, char** argv)
{
    // The parent may vanish at any time; writes then fail with EPIPE/EOF
    // handling in the socket layer instead of killing the process.
    signal(SIGPIPE, SIG_IGN);

    int socketFd = -1;
    std::string bootstrapName;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--socket-fd") == 0 && i + 1 < argc)
        {
            socketFd = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--bootstrap-name") == 0 && i + 1 < argc)
        {
            bootstrapName = argv[++i];
        }
        else
        {
            fprintf(stderr, "usage: %s --socket-fd N --bootstrap-name NAME\n", argv[0]);
            return 64;
        }
    }
    if (socketFd < 0 || bootstrapName.empty())
    {
        fprintf(stderr, "usage: %s --socket-fd N --bootstrap-name NAME\n", argv[0]);
        return 64;
    }

    @autoreleasepool
    {
        oxrsys::encoder::helper::EncoderHelperServer server;
        return server.Run(socketFd, bootstrapName);
    }
}
