// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "peer-filter.h"

#include <kj/debug.h>

#if _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstring>

namespace workerd::rust::kj_hyper {

// kj::LowLevelAsyncIoProvider::NetworkFilter::shouldAllow() is declared non-const by the kj
// interface even though it only reads the rules, and HyperPeerFilter::shouldAllow() is const
// (Rust reaches it through a shared reference); a const kj::Rc yields a const pointee, so recover
// the non-const object here. The filter is immutable after construction.
kj_rs_io::PeerFilter& HyperPeerFilter::filter() const {
  return const_cast<kj_rs_io::PeerFilter&>(*active);
}

bool HyperPeerFilter::shouldAllow(
    bool isIpv6, ::rust::Slice<const uint8_t> addr, uint16_t port) const {
  // Build the sockaddr kj_rs_io::PeerFilter::shouldAllow() expects, from the resolved IP's
  // network-order bytes. IP octets arrive already in network byte order; the port is host order.
  if (isIpv6) {
    KJ_REQUIRE(addr.size() == 16, "IPv6 address must be 16 bytes", addr.size());
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    memcpy(&sa.sin6_addr, addr.data(), 16);
    return filter().shouldAllow(reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));
  } else {
    KJ_REQUIRE(addr.size() == 4, "IPv4 address must be 4 bytes", addr.size());
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, addr.data(), 4);
    return filter().shouldAllow(reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));
  }
}

std::unique_ptr<HyperPeerFilter> newAllowAllHyperPeerFilter() {
  return std::make_unique<HyperPeerFilter>();
}

std::unique_ptr<HyperPeerFilter> newHyperPeerFilter(
    kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) {
  return std::make_unique<HyperPeerFilter>(allow, deny);
}

}  // namespace workerd::rust::kj_hyper
