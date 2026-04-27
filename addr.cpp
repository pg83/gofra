#include "addr.h"

#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>
#include <std/dbg/verify.h>

#include <arpa/inet.h>
#include <netinet/in.h>

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

sockaddr_in gofra::makeAddr(u32 ip, u16 port) {
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(ip);
    return sa;
}

sockaddr_in gofra::parseSockAddr(StringView s) {
    StringView ipPart, portPart;

    if (!s.split(':', ipPart, portPart)) {
        raise(StringBuilder() << StringView(u8"expected ip:port, got: ") << s);
    }

    return makeAddr(parseIPv4(ipPart), (u16)portPart.stou());
}
