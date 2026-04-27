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
    void udpReader(int udpFd, int tunFd, UdpReaderScratch* sc);
}
