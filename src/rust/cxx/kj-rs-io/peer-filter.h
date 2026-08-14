#pragma once
// PeerFilter: a faithful port of KJ's kj::_::NetworkFilter (kj/async-io.c++), backing
// kj-rs-io's Network::restrictPeers() support.
//
// Ported rather than reused because kj::_::NetworkFilter lives in KJ's internal header
// (kj/async-io-internal.h), whose quoted includes ("vector.h") only resolve inside the KJ
// source tree — it is not includable through Bazel's virtual include dirs. The allow/deny
// grammar ("public"/"private"/"local"/"network"/"unix"/"unix-abstract"/CIDRs), the RFC CIDR
// tables, the specificity tie-breaking between allow and deny rules, and the filter-chaining
// semantics are kept identical so restrictPeers() behaves exactly like kj::setupAsyncIo()'s
// networks. kj::CidrRange itself IS reused (kj/cidr.h is a clean public header).

#include <kj/async-io.h>
#include <kj/cidr.h>
#include <kj/vector.h>

namespace kj_rs_io {

class PeerFilter final: public kj::LowLevelAsyncIoProvider::NetworkFilter {
 public:
  // Allow-everything filter (matches KJ's root networks).
  PeerFilter();

  // Restriction layered on `next` (which must outlive this filter). Grammar identical to
  // kj::Network::restrictPeers().
  PeerFilter(kj::ArrayPtr<const kj::StringPtr> allow,
      kj::ArrayPtr<const kj::StringPtr> deny,
      PeerFilter &next);

  // Read-only despite the non-const signature: this override matches
  // kj::LowLevelAsyncIoProvider::NetworkFilter::shouldAllow (declared non-const upstream), but the
  // implementation only *reads* the CIDR tables / flags and recurses into `next` — it mutates no
  // member and has no interior mutability, so concurrent callers sharing a filter are safe. Keep
  // it read-only.
  bool shouldAllow(const struct sockaddr *addr, kj::uint addrlen) override;

  // Immobile like KJ's own kj::_::NetworkFilter: a restricted filter's `next` points at a parent
  // filter, and derived (restrictPeers) filters point back at this one, so a move would dangle the
  // chain. Every instance is either an owning member of a heap-allocated network/provider or
  // kj::heap<PeerFilter>(), so nothing moves one; this guards the latent hazard.
  KJ_DISALLOW_COPY_AND_MOVE(PeerFilter);

 private:
  kj::Vector<kj::CidrRange> allowCidrs;
  kj::Vector<kj::CidrRange> denyCidrs;
  bool allowUnix;
  bool allowAbstractUnix;
  bool allowPublic = false;
  bool allowNetwork = false;

  kj::Maybe<PeerFilter &> next;
};

}  // namespace kj_rs_io
