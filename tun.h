#pragma once

#include <std/sys/types.h>

namespace gofra {
    // openTun opens /dev/net/tun, names the device, sets MTU, assigns
    // the inner IPv4 / prefix, and brings the link up. Returns the
    // file descriptor (kept open for the lifetime of the process).
    //
    // Phase 1: single queue, no IFF_VNET_HDR. Carries plain inner IP
    // packets at MTU; no kernel-side TSO/GSO. Phase 2 will add multi-
    // queue + virtio_net_hdr + TUNSETOFFLOAD for parity with the Go
    // gofra1 perf profile.
    int openTun(const char* dev, int mtu, u32 vip, u8 prefixLen);
}
