#include "addr.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>
#include <std/dbg/verify.h>

#include <netdb.h>
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

    template <size_t N>
    void toCStr(char (&dst)[N], StringView s) {
        STD_VERIFY(s.length() < N);
        memCpy(dst, s.data(), s.length());
        dst[s.length()] = 0;
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
    toCStr(buf, s);

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

    if (!s.empty() && s[0] == '[') {
        StringView body = s.suffix(s.length() - 1);

        if (!body.split(']', ipPart, portPart) || portPart.empty() || portPart[0] != ':') {
            raise(StringBuilder() << StringView(u8"expected [ipv6]:port: ") << s);
        }

        portPart = portPart.suffix(portPart.length() - 1);
    } else if (!s.split(':', ipPart, portPart)) {
        raise(StringBuilder() << StringView(u8"expected ip:port: ") << s);
    }

    char host[INET6_ADDRSTRLEN];
    char serv[8];
    toCStr(host, ipPart);
    toCStr(serv, portPart);

    addrinfo hints = {};
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* res = nullptr;

    if (getaddrinfo(host, serv, &hints, &res) != 0) {
        raise(StringBuilder() << StringView(u8"bad ip:port: ") << s);
    }

    auto* ss = pool->make<sockaddr_storage>();
    memCpy(ss, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

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
