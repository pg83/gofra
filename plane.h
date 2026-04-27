#pragma once

namespace stl {
    struct IoReactor;
}

namespace gofra {
    class PeerTable;

    // tunReader: read inner IP packets off `tunFd`, look up the
    // destination peer in `peers`, RR-pick an underlay dst, and
    // sendto via `udpFd`. Phase 1 uses a single UDP socket and a
    // single TUN queue; later phases will fan out across N pairs.
    void tunReader(stl::IoReactor* reactor, int tunFd, int udpFd, PeerTable* peers, int mtu);

    // udpReader: recvfrom on `udpFd` and write each datagram into
    // `tunFd`. No seq prefix on the wire — the kernel TCP / SACK
    // handles cross-NIC reorder. Phase 2 will widen this to N→N.
    void udpReader(stl::IoReactor* reactor, int udpFd, int tunFd);
}
