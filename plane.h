#pragma once

namespace stl {
    class ObjPool;
    struct IoReactor;
}

namespace gofra {
    struct ConnTable;

    // Per-tunReader scratch space: 64 KiB GSO read buffer + 64 × MTU
    // segment output buffers. Defined privately in plane.cpp; main
    // gets an opaque pointer back from makeTunReaderScratch and
    // hands it to the coroutine. Allocation happens once on the
    // main thread before any coroutine runs — ObjPool isn't
    // thread-safe.
    struct TunReaderScratch;

    TunReaderScratch* makeTunReaderScratch(stl::ObjPool* pool);

    // tunReader: read inner IP packets off `tunFd`. Each read carries
    // a 10-byte virtio_net_hdr prefix; on GSO_TCPV4 we split the
    // super-packet via gsoSplit and send each segment, on GSO_NONE
    // we send as-is (after fixing up the L4 checksum if NEEDS_CSUM).
    // Stripe slot picked from `conns` per packet sent. `scratch`
    // owns all the working buffers.
    void tunReader(stl::IoReactor* reactor, int tunFd, ConnTable* conns,
                   TunReaderScratch* scratch);

    // udpReader: recvfrom on `udpFd` and write each datagram into
    // `tunFd` with a zeroed 10-byte virtio_net_hdr prefix (= no
    // offload, plain packet). main spawns one of these per
    // ConnTable::srcFd(i).
    void udpReader(stl::IoReactor* reactor, int udpFd, int tunFd);
}
