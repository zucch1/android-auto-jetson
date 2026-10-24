// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <string>

namespace wireless_test {
inline std::string host_endpoint() {
    ifaddrs* interfaces{};
    if (::getifaddrs(&interfaces) != 0) return {};
    std::string result;
    for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0 || (entry->ifa_flags & IFF_UP) == 0) continue;
        char text[INET_ADDRSTRLEN]{};
        const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) { result = text; break; }
    }
    ::freeifaddrs(interfaces);
    return result;
}
}
