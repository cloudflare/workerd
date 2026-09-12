#pragma once
// PeerFilter: kj-rs-io's restrictPeers() filter -- KJ's own kj::_::NetworkFilter (the policy
// behind kj::setupAsyncIo()'s networks: the "public"/"private"/"local"/"network"/"unix"/
// "unix-abstract"/CIDR grammar, the RFC CIDR tables, allow/deny specificity tie-breaking, and
// filter chaining) with one thing added: ownership of the chain.
//
// KJ's NetworkFilter holds its parent by reference and leaves keeping the parent alive to the
// caller (kj::Network implementations own their parent network by convention). kj-rs-io hands
// filters to objects with independent lifetimes -- networks, addresses, receivers, connect()
// promises that may outlive the network -- so the chain is refcounted instead: a PeerFilter owns
// a kj::Rc share of its parent, and every holder shares ownership via kj::Rc. No behavior of the
// policy itself lives here.

#include <kj/async-io-internal.h>
#include <kj/async-io.h>
#include <kj/refcount.h>

namespace kj_rs_io {

class PeerFilter final: public kj::LowLevelAsyncIoProvider::NetworkFilter, public kj::Refcounted {
 public:
  // Allow-everything filter (matches KJ's root networks).
  PeerFilter() = default;

  // Restriction layered on `next`, which the new filter OWNS: a restrictPeers() chain keeps its
  // parent filters alive, so there is no outlive-me contract between networks. Grammar identical
  // to kj::Network::restrictPeers() (it IS kj's parser).
  PeerFilter(kj::ArrayPtr<const kj::StringPtr> allow,
      kj::ArrayPtr<const kj::StringPtr> deny,
      kj::Rc<PeerFilter> next);

  // Read-only despite the non-const signature: this override matches
  // kj::LowLevelAsyncIoProvider::NetworkFilter::shouldAllow (declared non-const upstream), and
  // kj::_::NetworkFilter::shouldAllow only *reads* its CIDR tables / flags and recurses into
  // `next` -- no member is mutated and there is no interior mutability, so concurrent callers
  // sharing a filter are safe.
  bool shouldAllow(const struct sockaddr *addr, kj::uint addrlen) override;

  // KJ's parse-time check (kj::_::NetworkFilter::shouldAllowParse): whether a *literal* address
  // handed to kj::Network::parseAddress() may be kept at all. It judges the address family only
  // (an "allow public" filter rejects no IP literal at parse time; the literal's class is judged
  // at connect()), and KJ applies it to literals, never to DNS results -- kj's lookupHost()
  // filters nothing. TokioNetwork::parseAddress does exactly the same.
  // `const` because Rust reaches it through a shared `KjRc<PeerFilter>` and it only reads; see
  // `impl` below.
  bool shouldAllowParse(const struct sockaddr *addr, kj::uint addrlen) const;

  // Refcounted (always created via kj::rc<PeerFilter>()) and immobile: every holder -- networks,
  // addresses, receivers, derived filters' `next` -- shares ownership via kj::Rc, so a filter can
  // never be destroyed while anything still routes through it.
  KJ_DISALLOW_COPY_AND_MOVE(PeerFilter);

 private:
  // Declared before `impl`, which refers to `next->impl`: the share must exist first and be
  // destroyed last.
  kj::Rc<PeerFilter> next;
  // `mutable` only because kj::_::NetworkFilter declares its (purely reading) query methods
  // non-const; nothing here mutates it after construction.
  mutable kj::_::NetworkFilter impl;
};

}  // namespace kj_rs_io
