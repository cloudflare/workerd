// Port of KJ's kj::_::NetworkFilter (kj/async-io.c++, MIT-licensed, Sandstorm Development
// Group and contributors) — see peer-filter.h for why this is a port rather than a reuse.
// Behavior must be kept in lockstep with upstream KJ.

#include "kj-rs-io/peer-filter.h"

#include <kj/debug.h>

#if _WIN32
#include <winsock2.h>
// windows.h (pulled in by winsock2.h) defines ERROR as a macro, which breaks KJ_LOG(ERROR).
#include <kj/windows-sanity.h>
#else
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace kj_rs_io {
namespace {

using kj::CidrRange;

kj::ArrayPtr<const CidrRange> localCidrs() {
  static const CidrRange result[] = {
    // localhost
    "127.0.0.0/8"_kj,
    "::1/128"_kj,

    // Trying to *connect* to 0.0.0.0 on many systems is equivalent to connecting to
    // localhost. (wat)
    "0.0.0.0/32"_kj,
    "::/128"_kj,
  };
  return kj::arrayPtr(result, kj::size(result));
}

kj::ArrayPtr<const CidrRange> privateCidrs() {
  static const CidrRange result[] = {
    "10.0.0.0/8"_kj,      // RFC1918 reserved for internal network
    "100.64.0.0/10"_kj,   // RFC6598 "shared address space" for carrier-grade NAT
    "169.254.0.0/16"_kj,  // RFC3927 "link local" (auto-configured LAN in absence of DHCP)
    "172.16.0.0/12"_kj,   // RFC1918 reserved for internal network
    "192.168.0.0/16"_kj,  // RFC1918 reserved for internal network

    "fc00::/7"_kj,   // RFC4193 unique private network
    "fe80::/10"_kj,  // RFC4291 "link local" (auto-configured LAN in absence of DHCP)
  };
  return kj::arrayPtr(result, kj::size(result));
}

kj::ArrayPtr<const CidrRange> reservedCidrs() {
  // Address ranges reserved by RFCs for specific alternative protocols. These are not
  // considered part of "public", "private", "network", nor "local". But, we will allow apps to
  // explicitly allowlist CIDRs in this range if they really want, because some people actually
  // use these ranges as if they were private ranges.
  static const CidrRange result[] = {
    "192.0.0.0/24"_kj,        // RFC6890 reserved for special protocols
    "224.0.0.0/4"_kj,         // RFC1112 multicast
    "240.0.0.0/4"_kj,         // RFC1112 multicast / reserved for future use
    "255.255.255.255/32"_kj,  // RFC0919 broadcast address

    "2001::/23"_kj,  // RFC2928 reserved for special protocols
    "ff00::/8"_kj,   // RFC4291 multicast
  };
  return kj::arrayPtr(result, kj::size(result));
}

bool matchesAny(kj::ArrayPtr<const CidrRange> cidrs, const struct sockaddr *addr) {
  for (auto &cidr: cidrs) {
    if (cidr.matches(addr)) return true;
  }
  return false;
}

#if !_WIN32
// sockaddr_un::sun_path is not required to have a NUL terminator, so it must be read carefully.
kj::ArrayPtr<const char> safeUnixPath(const struct sockaddr_un *addr, kj::uint addrlen) {
  KJ_REQUIRE(addr->sun_family == AF_UNIX, "not a unix address");
  KJ_REQUIRE(addrlen >= offsetof(sockaddr_un, sun_path), "invalid unix address");

  size_t maxPathlen = addrlen - offsetof(sockaddr_un, sun_path);

  size_t pathlen;
  if (maxPathlen > 0 && addr->sun_path[0] == '\0') {
    // Linux "abstract" unix address
    pathlen = strnlen(addr->sun_path + 1, maxPathlen - 1) + 1;
  } else {
    pathlen = strnlen(addr->sun_path, maxPathlen);
  }
  return kj::arrayPtr(addr->sun_path, pathlen);
}
#endif  // !_WIN32

}  // namespace

PeerFilter::PeerFilter(): allowUnix(true), allowAbstractUnix(true) {
  allowCidrs.add(CidrRange::inet4({0, 0, 0, 0}, 0));
  allowCidrs.add(CidrRange::inet6({}, {}, 0));
}

PeerFilter::PeerFilter(kj::ArrayPtr<const kj::StringPtr> allow,
    kj::ArrayPtr<const kj::StringPtr> deny,
    kj::Rc<PeerFilter> next)
    : allowUnix(false),
      allowAbstractUnix(false),
      next(kj::mv(next)) {
  for (auto rule: allow) {
    if (rule == "local") {
      allowCidrs.addAll(localCidrs());
    } else if (rule == "network") {
      // Can't be represented as a simple union of CIDRs, so we handle in shouldAllow().
      allowNetwork = true;
    } else if (rule == "private") {
      allowCidrs.addAll(privateCidrs());
      allowCidrs.addAll(localCidrs());
    } else if (rule == "public") {
      // Can't be represented as a simple union of CIDRs, so we handle in shouldAllow().
      allowPublic = true;
    } else if (rule == "unix") {
      allowUnix = true;
    } else if (rule == "unix-abstract") {
      allowAbstractUnix = true;
    } else {
      allowCidrs.add(CidrRange(rule));
    }
  }

  for (auto rule: deny) {
    if (rule == "local") {
      denyCidrs.addAll(localCidrs());
    } else if (rule == "network") {
      KJ_FAIL_REQUIRE("don't deny 'network', allow 'local' instead");
    } else if (rule == "private") {
      denyCidrs.addAll(privateCidrs());
    } else if (rule == "public") {
      // Tricky: What if we allow 'network' and deny 'public'?
      KJ_FAIL_REQUIRE("don't deny 'public', allow 'private' instead");
    } else if (rule == "unix") {
      allowUnix = false;
    } else if (rule == "unix-abstract") {
      allowAbstractUnix = false;
    } else {
      denyCidrs.add(CidrRange(rule));
    }
  }
}

bool PeerFilter::shouldAllow(const struct sockaddr *addr, kj::uint addrlen) {
  KJ_REQUIRE(addrlen >= sizeof(addr->sa_family));

#if !_WIN32
  if (addr->sa_family == AF_UNIX) {
    auto path = safeUnixPath(reinterpret_cast<const struct sockaddr_un *>(addr), addrlen);
    if (path.size() > 0 && path[0] == '\0') {
      return allowAbstractUnix;
    } else {
      return allowUnix;
    }
  }
#endif

  bool allowed = false;
  kj::uint allowSpecificity = 0;

  if (allowPublic) {
    if ((addr->sa_family == AF_INET || addr->sa_family == AF_INET6) &&
        !matchesAny(privateCidrs(), addr) && !matchesAny(localCidrs(), addr) &&
        !matchesAny(reservedCidrs(), addr)) {
      allowed = true;
      // Don't adjust allowSpecificity as this match has an effective specificity of zero.
    }
  }

  if (allowNetwork) {
    if ((addr->sa_family == AF_INET || addr->sa_family == AF_INET6) &&
        !matchesAny(localCidrs(), addr) && !matchesAny(reservedCidrs(), addr)) {
      allowed = true;
      // Don't adjust allowSpecificity as this match has an effective specificity of zero.
    }
  }

  for (auto &cidr: allowCidrs) {
    if (cidr.matches(addr)) {
      allowSpecificity = kj::max(allowSpecificity, cidr.getSpecificity());
      allowed = true;
    }
  }
  if (!allowed) return false;
  for (auto &cidr: denyCidrs) {
    if (cidr.matches(addr)) {
      if (cidr.getSpecificity() >= allowSpecificity) return false;
    }
  }

  KJ_IF_SOME(n, next) {
    return n->shouldAllow(addr, addrlen);
  } else {
    return true;
  }
}

}  // namespace kj_rs_io
