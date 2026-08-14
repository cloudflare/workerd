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
#include <kj/refcount.h>
#include <kj/vector.h>

namespace kj_rs_io {

class PeerFilter final: public kj::LowLevelAsyncIoProvider::NetworkFilter, public kj::Refcounted {
 public:
  // Allow-everything filter (matches KJ's root networks).
  PeerFilter();

  // Restriction layered on `next`, which the new filter OWNS: a restrictPeers() chain keeps its
  // parent filters alive, so there is no outlive-me contract between networks. Grammar identical
  // to kj::Network::restrictPeers().
  PeerFilter(kj::ArrayPtr<const kj::StringPtr> allow,
      kj::ArrayPtr<const kj::StringPtr> deny,
      kj::Rc<PeerFilter> next);

  // Read-only despite the non-const signature: this override matches
  // kj::LowLevelAsyncIoProvider::NetworkFilter::shouldAllow (declared non-const upstream), but the
  // implementation only *reads* the CIDR tables / flags and recurses into `next` — it mutates no
  // member and has no interior mutability, so concurrent callers sharing a filter are safe. Keep
  // it read-only.
  bool shouldAllow(const struct sockaddr *addr, kj::uint addrlen) override;

  // Refcounted (always created via kj::rc<PeerFilter>()) and immobile: every holder — networks,
  // addresses, receivers, derived filters' `next` — shares ownership via kj::Rc, so a filter can
  // never be destroyed or moved out from under its chain.
  KJ_DISALLOW_COPY_AND_MOVE(PeerFilter);

 private:
  kj::Vector<kj::CidrRange> allowCidrs;
  kj::Vector<kj::CidrRange> denyCidrs;
  bool allowUnix;
  bool allowAbstractUnix;
  bool allowPublic = false;
  bool allowNetwork = false;

  kj::Maybe<kj::Rc<PeerFilter>> next;
};

}  // namespace kj_rs_io
