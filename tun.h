#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace gofra {
    // Attach one IFF_MULTI_QUEUE | IFF_VNET_HDR queue on `dev`; fd
    // pool-tracked. Call N times for N parallel queues.
    int openTun(stl::ObjPool* pool, const char* dev);

    // Iface-level netlink: MTU + addr + up. Call once after all
    // queues attached.
    void configureTun(const char* dev, int mtu, u32 vip, u8 prefixLen);
}
