#pragma once

#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    int openUdpSocket(stl::ObjPool* pool, const sockaddr* src, int rcvBuf, int sndBuf);
}
