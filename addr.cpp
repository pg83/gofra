#include "addr.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>
#include <std/dbg/verify.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace stl;

namespace {
    struct AddrError: public Exception {
        Buffer msg_;
        Buffer full_;

        AddrError(Buffer&& m) noexcept
            : msg_(static_cast<Buffer&&>(m))
        {
        }

        ExceptionKind kind() const noexcept override {
            return ExceptionKind::Verify;
        }

        StringView description() override;
    };

    [[noreturn]] void raise(Buffer&& msg) {
        throw AddrError(static_cast<Buffer&&>(msg));
    }
}

StringView AddrError::description() {
    if (!full_.empty()) {
        return full_;
    }

    (StringBuilder()
     << StringView(u8"addr: ")
     << msg_)
        .xchg(full_);

    return full_;
}

u32 gofra::parseIPv4(StringView s) {
    char buf[16];
    STD_VERIFY(s.length() < sizeof(buf));
    memCpy(buf, s.data(), s.length());
    buf[s.length()] = 0;

    in_addr a;

    if (inet_pton(AF_INET, buf, &a) != 1) {
        raise(StringBuilder() << StringView(u8"bad ipv4: ") << s);
    }

    return ntohl(a.s_addr);
}

void gofra::parseCIDR(StringView s, u32* addr, u8* prefixLen) {
    StringView before, after;

    if (s.split('/', before, after)) {
        *addr = parseIPv4(before);
        *prefixLen = (u8)after.stou();
    } else {
        *addr = parseIPv4(s);
        *prefixLen = 32;
    }
}

const sockaddr* gofra::parseSockAddr(ObjPool* pool, StringView s) {
    StringView ipPart, portPart;

    if (!s.split(':', ipPart, portPart)) {
        raise(StringBuilder() << StringView(u8"expected ip:port, got: ") << s);
    }

    u32 ip = parseIPv4(ipPart);
    u16 port = (u16)portPart.stou();

    auto* ss = pool->make<sockaddr_storage>();
    auto* sin = (sockaddr_in*)ss;

    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = htonl(ip);

    return (const sockaddr*)ss;
}

u32 gofra::addrLen(const sockaddr* sa) noexcept {
    switch (sa->sa_family) {
        case AF_INET:
            return sizeof(sockaddr_in);
        case AF_INET6:
            return sizeof(sockaddr_in6);
    }

    return sizeof(sockaddr_storage);
}
