#pragma once

namespace stl {
    struct IoReactor;
}

namespace gofra {
    struct ConnTable;

    // tunReader: read inner IP packets off `tunFd`, look up the
    // destination's Conn in `conns`, take the next stripe slot, and
    // sendto via slot->srcFd toward slot->dstAddr. One coroutine,
    // serializes against itself; the per-Conn counter is atomic so
    // future multi-tunReader is safe.
    void tunReader(stl::IoReactor* reactor, int tunFd, ConnTable* conns, int mtu);

    // udpReader: recvfrom on `udpFd` and write each datagram into
    // `tunFd`. main spawns one of these per ConnTable::srcFd(i).
    void udpReader(stl::IoReactor* reactor, int udpFd, int tunFd);
}
