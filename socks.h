#pragma once

#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    // openUdpSocket binds (src), pins egress to the iface that owns
    // src via SO_BINDTODEVICE, bumps SO_RCVBUFFORCE / SO_SNDBUFFORCE
    // to the configured size. The fd is registered in `pool` for
    // ::close on pool destruction; the returned int is the live fd.
    int openUdpSocket(stl::ObjPool* pool, const sockaddr* src, int rcvBuf, int sndBuf);
}
