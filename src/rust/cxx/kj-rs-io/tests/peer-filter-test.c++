// Unit tests for what PeerFilter adds over kj::_::NetworkFilter (which it wraps, not
// reimplements): ownership of the chain, a chain's verdict, atomic sharing across threads, plus
// one grammar smoke test proving the wrapper forwards to KJ's policy at all. KJ's own grammar is
// KJ's to test.

#include "kj-rs-io/peer-filter.h"

#include <kj/debug.h>
#include <kj/test.h>
#include <kj/thread.h>

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
kj::Arc<PeerFilter> filter(
    kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny = nullptr) {
  return kj::arc<PeerFilter>(allow, deny, kj::arc<PeerFilter>());
}

// Does `f` allow the given numeric IP (v4 if it has no ':', else v6), port 1?
bool allowsIp(const PeerFilter& f, kj::StringPtr ip) {
  if (ip.findFirst(':') != kj::none) {
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(1);
    KJ_ASSERT(inet_pton(AF_INET6, ip.cStr(), &sin6.sin6_addr) == 1, ip);
    return f.allows(reinterpret_cast<struct sockaddr*>(&sin6), sizeof(sin6));
  } else {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(1);
    KJ_ASSERT(inet_pton(AF_INET, ip.cStr(), &sin.sin_addr) == 1, ip);
    return f.allows(reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
  }
}

KJ_TEST("PeerFilter: default filter allows everything") {
  auto f = kj::arc<PeerFilter>();
  KJ_EXPECT(allowsIp(*f, "8.8.8.8"));
  KJ_EXPECT(allowsIp(*f, "127.0.0.1"));
  KJ_EXPECT(allowsIp(*f, "10.0.0.1"));
  KJ_EXPECT(allowsIp(*f, "::1"));
}

KJ_TEST("PeerFilter: forwards to KJ's grammar ('public' allows public IPs, blocks the rest)") {
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

KJ_TEST("PeerFilter: nested filter chain enforces BOTH levels") {
  // Child allows all private; parent (next) allows only local. An address the child allows but
  // the parent denies must be blocked — the chain is an AND.
  auto parent = kj::arc<PeerFilter>(kj::arr("local"_kj), nullptr, kj::arc<PeerFilter>());
  auto child = kj::arc<PeerFilter>(kj::arr("private"_kj), nullptr, kj::mv(parent));
  KJ_EXPECT(allowsIp(*child, "127.0.0.1"));  // allowed by both child (private⊇local) and parent
  KJ_EXPECT(!allowsIp(*child, "10.0.0.1"));  // allowed by child, DENIED by parent → blocked
}

KJ_TEST("PeerFilter: shares are atomic -- taking and dropping them on two threads is race-free") {
  // kj::NetworkAddress::clone() takes a share of its network's filter, and KJ allows an address
  // to be cloned on another thread. A kj::Rc's count would race here (a TSAN finding in review);
  // kj::Arc's does not. Run under --config=tsan-macos / tsan to check, not just to pass.
  auto root = kj::arc<PeerFilter>();
  auto child = kj::arc<PeerFilter>(kj::arr("private"_kj), nullptr, root.addRef());
  auto churn = [](kj::Arc<PeerFilter> filter) {
    for (int i = 0; i < 100000; i++) {
      auto share = filter.addRef();
      KJ_EXPECT(allowsIp(*share, "10.0.0.1"));
    }
  };
  kj::Thread other([&, filter = child.addRef()]() mutable { churn(kj::mv(filter)); });
  churn(kj::mv(child));
}

#if !_WIN32
// Builds a sockaddr_un for `path` (abstract if `abstractLeadingNul`) and returns the verdict.
bool allowsUnix(const PeerFilter& f, kj::StringPtr path, bool abstractLeadingNul = false) {
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
  return f.allows(reinterpret_cast<struct sockaddr*>(&su), addrlen);
}

KJ_TEST("PeerFilter: unix and unix-abstract allow/deny") {
  // Default allows both.
  auto def = kj::arc<PeerFilter>();
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
  auto denyUnix = kj::arc<PeerFilter>(
      kj::arr("private"_kj, "unix"_kj), kj::arr("unix"_kj), kj::arc<PeerFilter>());
  KJ_EXPECT(!allowsUnix(*denyUnix, "/tmp/sock"));
}

KJ_TEST("PeerFilter: unix socket permissions are decided by the innermost filter (KJ behavior)") {
  // KJ's NetworkFilter::shouldAllow answers AF_UNIX from its own allowUnix/allowAbstractUnix
  // flags without consulting `next` (kj/async-io.c++), so a child that allows unix sockets
  // allows them even under a parent that only allows "public". This IS kj::setupAsyncIo()'s
  // behavior -- PeerFilter is that implementation -- and workerd's restrictPeers callers get
  // exactly it under either backend. (Whether the chain *should* intersect here is an upstream
  // KJ question, not something the tokio backend may answer differently.)
  auto parent = kj::arc<PeerFilter>(kj::arr("public"_kj), nullptr, kj::arc<PeerFilter>());
  auto child = kj::arc<PeerFilter>(kj::arr("unix"_kj, "unix-abstract"_kj), nullptr, kj::mv(parent));

  KJ_EXPECT(allowsUnix(*child, "/tmp/sock"));
  KJ_EXPECT(allowsUnix(*child, "abstract-name", true));

  // ...whereas a child that does not name unix sockets rejects them regardless of the parent.
  auto ipOnlyChild = kj::arc<PeerFilter>(kj::arr("private"_kj), nullptr, kj::arc<PeerFilter>());
  KJ_EXPECT(!allowsUnix(*ipOnlyChild, "/tmp/sock"));
}
#endif  // !_WIN32

}  // namespace
}  // namespace kj_rs_io
