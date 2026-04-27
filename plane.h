#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace gofra {
    struct ConnTable;

    // tunReader scratch (64 KiB read + 64 MTU seg bufs); plane.cpp-private.
    struct TunReaderScratch;

    // udpReader scratch (64-deep mmsghdr/iovec/sockaddr* + buffers).
    struct UdpReaderScratch;

    TunReaderScratch* makeTunReaderScratch(stl::ObjPool* pool);
    UdpReaderScratch* makeUdpReaderScratch(stl::ObjPool* pool);

    // Blocking read(tunFd) → gsoSplit → sendUso through stripe slots.
    void tunReader(int tunFd, ConnTable* conns, TunReaderScratch* sc);

    // Blocking recvmmsg(64) → write [zero virtio_net_hdr | payload] to TUN.
    void udpReader(int udpFd, int tunFd, UdpReaderScratch* sc);
}
