#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace gofra {
    struct ConnTable;

    // Per-tunReader scratch (64 KiB read buffer + 64 × MTU segment
    // buffers for gsoSplit output). Defined privately in plane.cpp;
    // pool->make one before the thread starts.
    struct TunReaderScratch;

    // Per-udpReader scratch (64-deep mmsghdr / iovec / sockaddr*
    // table + 64 × (10 B virtio_net_hdr + jumbo payload) buffers,
    // pre-wired). Same lifetime story as TunReaderScratch.
    struct UdpReaderScratch;

    TunReaderScratch* makeTunReaderScratch(stl::ObjPool* pool);
    UdpReaderScratch* makeUdpReaderScratch(stl::ObjPool* pool);

    // tunReader: blocking read(tunFd) loop. Decodes virtio_net_hdr,
    // splits GSO_TCPV4 super-packets via gsoSplit, sends each segment
    // through the stripe slot picked from `conns`. Runs on its own
    // OS thread — no coroutine yield, no io_uring.
    void tunReader(int tunFd, ConnTable* conns, TunReaderScratch* sc);

    // udpReader: blocking recvmmsg(64) loop. For each packet drained
    // from the UDP socket, write[10-byte zeroed virtio_net_hdr |
    // payload] to the paired TUN queue.
    void udpReader(int udpFd, int tunFd, UdpReaderScratch* sc);
}
