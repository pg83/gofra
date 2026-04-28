#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace gofra {
    struct ConnTable;

    struct TunReaderScratch;
    struct UdpReaderScratch;

    TunReaderScratch* makeTunReaderScratch(stl::ObjPool* pool);
    UdpReaderScratch* makeUdpReaderScratch(stl::ObjPool* pool);

    void tunReader(int tunFd, ConnTable* conns, TunReaderScratch* sc);
    void udpReader(int udpFd, int tunFd, u32 srcIdx, ConnTable* conns, UdpReaderScratch* sc);

    // Single thread per gofra instance: in a loop, sendto a 0-byte UDP
    // probe via every (srcFd, dstAddr) slot. Receiver bumps slot.lastSeen
    // (RX path treats payloadLen<20 as a probe). intervalMs throttles
    // the loop.
    void prober(ConnTable* conns, u64 intervalMs);

    // Every intervalSec, log a one-line snapshot of every slot's
    // (src,dst,alive) on stderr.
    void slotsStats(ConnTable* conns, u64 timeoutMs, u64 intervalSec);
}
