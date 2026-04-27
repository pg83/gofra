#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace gofra {
    // Call N times for N multi-queue queues; once afterwards configureTun.
    int openTun(stl::ObjPool* pool, const char* dev);
    void configureTun(const char* dev, int mtu, u32 vip, u8 prefixLen);
}
