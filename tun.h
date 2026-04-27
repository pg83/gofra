#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace gofra {
    // openTun opens one TUN queue on `dev` with IFF_MULTI_QUEUE and
    // returns its fd (pool-registered via stl::ScopedFD). Call N
    // times to attach N parallel queues to the same device, then
    // call configureTun once — the iface-level mtu/addr/up doesn't
    // care which fd opened it.
    //
    // Phase 1: no IFF_VNET_HDR / GSO / TSO — plain inner IP packets
    // at MTU. Phase 2 will add the virtio_net_hdr offload path.
    int openTun(stl::ObjPool* pool, const char* dev);

    // configureTun runs the iface-level netlink config: set MTU,
    // assign vip/prefixLen, bring the link up. Call exactly once
    // after openTun has attached all queues.
    void configureTun(const char* dev, int mtu, u32 vip, u8 prefixLen);
}
