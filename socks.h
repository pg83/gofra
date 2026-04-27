#pragma once

#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    // Bind UDP src, pin egress to the NIC owning it (SO_BINDTODEVICE),
    // force RCV/SND buf sizes. fd registered in pool; returned live.
    int openUdpSocket(stl::ObjPool* pool, const sockaddr* src, int rcvBuf, int sndBuf);
}
