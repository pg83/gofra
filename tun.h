#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace gofra {
    // openTun opens /dev/net/tun, names the device, sets MTU, assigns
    // the inner IPv4 / prefix, and brings the link up. The fd is
    // registered in `pool` for ::close on pool destruction; the
    // returned int is the live fd for the caller to use.
    //
    // Phase 1: single queue, no IFF_VNET_HDR. Carries plain inner IP
    // packets at MTU; no kernel-side TSO/GSO. Phase 2 will add multi-
    // queue + virtio_net_hdr + TUNSETOFFLOAD for parity with the Go
    // gofra1 perf profile.
    int openTun(stl::ObjPool* pool, const char* dev, int mtu, u32 vip, u8 prefixLen);
}
