#pragma once

#include <std/sys/types.h>

struct sockaddr_in;

namespace gofra {
    // openUdpSocket binds (src), pins egress to the iface that owns
    // src via SO_BINDTODEVICE, bumps SO_RCVBUFFORCE / SO_SNDBUFFORCE
    // to the configured size. Returns the socket fd.
    int openUdpSocket(const sockaddr_in* src, int rcvBuf, int sndBuf);
}
