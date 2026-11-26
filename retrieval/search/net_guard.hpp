// net_guard.hpp: reject connections to non-public addresses (SSRF guard). C++17, header-only.
// Use from a libcurl CURLOPT_OPENSOCKETFUNCTION so it runs for EVERY connection (redirects, DNS results).
#pragma once
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace niyah::search {

inline bool ipv4_public(std::uint32_t a) {  // host byte order
    const unsigned o1 = (a >> 24) & 255u, o2 = (a >> 16) & 255u, o3 = (a >> 8) & 255u;
    if (o1 == 0 || o1 == 10 || o1 == 127) return false;
    if (o1 == 100 && o2 >= 64 && o2 <= 127) return false;   // CGNAT
    if (o1 == 169 && o2 == 254) return false;               // link-local + cloud metadata
    if (o1 == 172 && o2 >= 16 && o2 <= 31) return false;
    if (o1 == 192 && o2 == 168) return false;
    if (o1 == 192 && o2 == 0 && o3 == 0) return false;
    if (o1 == 198 && (o2 == 18 || o2 == 19)) return false;
    if (o1 >= 224) return false;                            // multicast, reserved, broadcast
    return true;
}

inline bool ipv6_public(const std::uint8_t *b) {  // 16 bytes
    static const std::uint8_t zero15[15] = {0};
    const bool first15zero = std::memcmp(b, zero15, 15) == 0;
    if (first15zero && b[15] <= 1) return false;            // :: and ::1
    auto v4 = [&](int at) {
        return ipv4_public(std::uint32_t(b[at]) << 24 | std::uint32_t(b[at + 1]) << 16 |
                           std::uint32_t(b[at + 2]) << 8 | std::uint32_t(b[at + 3]));
    };
    if (std::memcmp(b, zero15, 10) == 0 && b[10] == 0xff && b[11] == 0xff) return v4(12);  // ::ffff:a.b.c.d
    static const std::uint8_t nat64[12] = {0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0};
    if (std::memcmp(b, nat64, 12) == 0) return v4(12);                                      // 64:ff9b::/96
    if ((b[0] & 0xfe) == 0xfc) return false;                // fc00::/7 unique local
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return false;  // fe80::/10 link-local
    if (b[0] == 0xff) return false;                         // multicast
    return true;
}

inline bool sockaddr_public(const sockaddr *sa) {
    if (!sa) return false;
    if (sa->sa_family == AF_INET) {
        const auto *p = reinterpret_cast<const sockaddr_in *>(sa);
        return ipv4_public(ntohl(p->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const auto *p = reinterpret_cast<const sockaddr_in6 *>(sa);
        return ipv6_public(reinterpret_cast<const std::uint8_t *>(&p->sin6_addr));
    }
    return false;  // unknown family: refuse
}

}  // namespace niyah::search
