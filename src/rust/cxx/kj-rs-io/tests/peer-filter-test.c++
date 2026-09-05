// Direct unit tests for PeerFilter::shouldAllow — the faithful port of kj::_::NetworkFilter that
// backs restrictPeers(). The network-level tests (async-io-test.c++) only exercise
// {"public"}/{"private"} end to end; this file drives the grammar directly against hand-built
// sockaddrs (no event loop, no sockets), covering every rule class, the allow/deny specificity
// tie-break, IPv6, unix/unix-abstract, and filter chaining.

#include "kj-rs-io/peer-filter.h"

#include <kj/debug.h>
#include <kj/test.h>

#include <cstring>

#if _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <kj/windows-sanity.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace kj_rs_io {
namespace {

// A filter with `allow`/`deny` rules layered on an allow-everything parent, so the parent never
// changes the verdict and the rules under test are what decides.
kj::Rc<PeerFilter> filter(
    kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny = nullptr) {
  return kj::rc<PeerFilter>(allow, deny, kj::rc<PeerFilter>());
}

// Does `f` allow the given numeric IP (v4 if it has no ':', else v6), port 1?
bool allowsIp(PeerFilter& f, kj::StringPtr ip) {
  if (ip.findFirst(':') != kj::none) {
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(1);
    KJ_ASSERT(inet_pton(AF_INET6, ip.cStr(), &sin6.sin6_addr) == 1, ip);
    return f.shouldAllow(reinterpret_cast<struct sockaddr*>(&sin6), sizeof(sin6));
  } else {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(1);
    KJ_ASSERT(inet_pton(AF_INET, ip.cStr(), &sin.sin_addr) == 1, ip);
    return f.shouldAllow(reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
  }
}

KJ_TEST("PeerFilter: default filter allows everything") {
  auto f = kj::rc<PeerFilter>();
  KJ_EXPECT(allowsIp(*f, "8.8.8.8"));
  KJ_EXPECT(allowsIp(*f, "127.0.0.1"));
  KJ_EXPECT(allowsIp(*f, "10.0.0.1"));
  KJ_EXPECT(allowsIp(*f, "::1"));
}

KJ_TEST("PeerFilter: 'public' allows public IPs, blocks private/local/reserved") {
  auto f = filter({"public"_kj});
  KJ_EXPECT(allowsIp(*f, "8.8.8.8"));       // public
  KJ_EXPECT(allowsIp(*f, "1.1.1.1"));       // public
  KJ_EXPECT(!allowsIp(*f, "10.0.0.1"));     // RFC1918 private
  KJ_EXPECT(!allowsIp(*f, "192.168.1.1"));  // RFC1918 private
  KJ_EXPECT(!allowsIp(*f, "172.16.5.5"));   // RFC1918 private
  KJ_EXPECT(!allowsIp(*f, "127.0.0.1"));    // local
  KJ_EXPECT(!allowsIp(*f, "224.0.0.1"));    // reserved (multicast)
  KJ_EXPECT(!allowsIp(*f, "169.254.1.1"));  // link-local (private)
}

KJ_TEST("PeerFilter: 'network' allows public+private, blocks local/reserved") {
  auto f = filter({"network"_kj});
  KJ_EXPECT(allowsIp(*f, "8.8.8.8"));      // public
  KJ_EXPECT(allowsIp(*f, "10.0.0.1"));     // private — the difference from 'public'
  KJ_EXPECT(allowsIp(*f, "192.168.1.1"));  // private
  KJ_EXPECT(!allowsIp(*f, "127.0.0.1"));   // local
  KJ_EXPECT(!allowsIp(*f, "224.0.0.1"));   // reserved
}

KJ_TEST("PeerFilter: 'local' allows loopback only") {
  auto f = filter({"local"_kj});
  KJ_EXPECT(allowsIp(*f, "127.0.0.1"));
  KJ_EXPECT(allowsIp(*f, "127.5.5.5"));  // 127/8
  KJ_EXPECT(!allowsIp(*f, "8.8.8.8"));
  KJ_EXPECT(!allowsIp(*f, "10.0.0.1"));
}

KJ_TEST("PeerFilter: 'private' allows RFC1918 + local, blocks public") {
  auto f = filter({"private"_kj});
  KJ_EXPECT(allowsIp(*f, "10.1.2.3"));
  KJ_EXPECT(allowsIp(*f, "192.168.0.1"));
  KJ_EXPECT(allowsIp(*f, "127.0.0.1"));  // 'private' includes local
  KJ_EXPECT(!allowsIp(*f, "8.8.8.8"));
}

KJ_TEST("PeerFilter: explicit CIDR allow") {
  auto f = filter({"10.0.0.0/8"_kj});
  KJ_EXPECT(allowsIp(*f, "10.1.2.3"));
  KJ_EXPECT(allowsIp(*f, "10.255.255.255"));
  KJ_EXPECT(!allowsIp(*f, "11.0.0.1"));
  KJ_EXPECT(!allowsIp(*f, "8.8.8.8"));
}

KJ_TEST("PeerFilter: allow + more-specific deny (specificity tie-break)") {
  // allow 10/8 but deny the more-specific 10.1/16: 10.1.x blocked, other 10.x allowed.
  auto f = filter({"10.0.0.0/8"_kj}, {"10.1.0.0/16"_kj});
  KJ_EXPECT(allowsIp(*f, "10.2.3.4"));   // allowed by /8, no deny matches
  KJ_EXPECT(!allowsIp(*f, "10.1.2.3"));  // deny /16 is more specific than allow /8
  KJ_EXPECT(allowsIp(*f, "10.255.0.1"));
}

KJ_TEST("PeerFilter: equal-specificity deny wins over allow (>= tie-break)") {
  // Same prefix length on both sides: deny's `>=` means the deny wins.
  auto f = filter({"10.1.0.0/16"_kj}, {"10.1.0.0/16"_kj});
  KJ_EXPECT(!allowsIp(*f, "10.1.2.3"));
}

KJ_TEST("PeerFilter: IPv6 public/private/explicit rules") {
  auto pub = filter({"public"_kj});
  KJ_EXPECT(allowsIp(*pub, "2606:4700:4700::1111"));  // public v6
  KJ_EXPECT(!allowsIp(*pub, "::1"));                  // v6 loopback (local)
  KJ_EXPECT(!allowsIp(*pub, "fc00::1"));              // v6 unique-local (private)

  auto priv = filter({"private"_kj});
  KJ_EXPECT(allowsIp(*priv, "fc00::1"));  // fc00::/7 private
  KJ_EXPECT(allowsIp(*priv, "::1"));      // local included in private
  KJ_EXPECT(!allowsIp(*priv, "2606:4700:4700::1111"));

  auto cidr = filter({"fc00::/7"_kj});
  KJ_EXPECT(allowsIp(*cidr, "fc00::1"));
  KJ_EXPECT(!allowsIp(*cidr, "2606:4700:4700::1111"));
}

KJ_TEST("PeerFilter: nested filter chain enforces BOTH levels") {
  // Child allows all private; parent (next) allows only local. An address the child allows but
  // the parent denies must be blocked — the chain is an AND.
  auto parent = kj::rc<PeerFilter>(kj::arr("local"_kj), nullptr, kj::rc<PeerFilter>());
  auto child = kj::rc<PeerFilter>(kj::arr("private"_kj), nullptr, kj::mv(parent));
  KJ_EXPECT(allowsIp(*child, "127.0.0.1"));  // allowed by both child (private⊇local) and parent
  KJ_EXPECT(!allowsIp(*child, "10.0.0.1"));  // allowed by child, DENIED by parent → blocked
}

KJ_TEST("PeerFilter: denying 'network' or 'public' is rejected") {
  KJ_EXPECT_THROW_MESSAGE("don't deny 'network'",
      kj::rc<PeerFilter>(nullptr, kj::arr("network"_kj), kj::rc<PeerFilter>()));
  KJ_EXPECT_THROW_MESSAGE("don't deny 'public'",
      kj::rc<PeerFilter>(nullptr, kj::arr("public"_kj), kj::rc<PeerFilter>()));
}

#if !_WIN32
// Builds a sockaddr_un for `path` (abstract if `abstractLeadingNul`) and returns the verdict.
bool allowsUnix(PeerFilter& f, kj::StringPtr path, bool abstractLeadingNul = false) {
  struct sockaddr_un su;
  memset(&su, 0, sizeof(su));
  su.sun_family = AF_UNIX;
  size_t off = 0;
  if (abstractLeadingNul) {
    su.sun_path[0] = '\0';
    off = 1;
  }
  memcpy(su.sun_path + off, path.begin(), path.size());
  kj::uint addrlen =
      static_cast<kj::uint>(offsetof(struct sockaddr_un, sun_path) + off + path.size());
  return f.shouldAllow(reinterpret_cast<struct sockaddr*>(&su), addrlen);
}

KJ_TEST("PeerFilter: unix and unix-abstract allow/deny") {
  // Default allows both.
  auto def = kj::rc<PeerFilter>();
  KJ_EXPECT(allowsUnix(*def, "/tmp/sock"));
  KJ_EXPECT(allowsUnix(*def, "abstract-name", true));

  // "unix" allows pathname sockets, not abstract; "unix-abstract" the reverse.
  auto pathOnly = filter({"unix"_kj});
  KJ_EXPECT(allowsUnix(*pathOnly, "/tmp/sock"));
  KJ_EXPECT(!allowsUnix(*pathOnly, "abstract-name", true));

  auto abstractOnly = filter({"unix-abstract"_kj});
  KJ_EXPECT(!allowsUnix(*abstractOnly, "/tmp/sock"));
  KJ_EXPECT(allowsUnix(*abstractOnly, "abstract-name", true));

  // Deny turns them off even from the allow-everything default.
  auto denyUnix = kj::rc<PeerFilter>(
      kj::arr("private"_kj, "unix"_kj), kj::arr("unix"_kj), kj::rc<PeerFilter>());
  KJ_EXPECT(!allowsUnix(*denyUnix, "/tmp/sock"));
}
#endif  // !_WIN32

}  // namespace
}  // namespace kj_rs_io
