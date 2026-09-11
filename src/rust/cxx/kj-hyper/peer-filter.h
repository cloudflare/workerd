// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// restrictPeers network ACL for the hyper outbound client. The hyper client dials host:port on
// tokio itself, so — unlike kj's own client, where kj::Network::connect() applies restrictPeers
// inside every connection — the filter must be re-applied at the hyper dial. This wraps
// kj_rs_io::PeerFilter (KJ's kj::_::NetworkFilter port, shared with the inbound/accept side) so
// the same allow/deny grammar and CIDR tables kj enforces govern every hyper connection, at the
// resolved IP actually being connected to. See client.rs `dial_filtered` for the resolve→check→
// connect sequence that makes the check atomic with the dial (closing the DNS-rebinding gap).

#include <kj-rs-io/peer-filter.h>
#include <rust/cxx.h>

#include <kj/common.h>
#include <kj/memory.h>

#include <cstdint>
#include <memory>

namespace workerd::rust::kj_hyper {

// Owns an allow-everything root filter plus, when restricted, a PeerFilter layered on it (kj_rs_io's
// PeerFilter is refcounted and chains to its parent through a kj::Rc). Immutable after
// construction; shouldAllow() only reads, so a single instance is shared across a client's dial
// tasks (see SharedFilter in client.rs).
class HyperPeerFilter {
 public:
  // Allow-everything (parity with kj's unrestricted root network — used for external services
  // and any hyper client with no restrictPeers configured).
  HyperPeerFilter(): active(kj::rc<kj_rs_io::PeerFilter>()) {}

  // Restriction with kj's allow/deny grammar ("public"/"private"/"local"/"network"/CIDRs/...),
  // identical to kj::Network::restrictPeers().
  HyperPeerFilter(kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny)
      : active(kj::rc<kj_rs_io::PeerFilter>(allow, deny, kj::rc<kj_rs_io::PeerFilter>())) {}

  KJ_DISALLOW_COPY_AND_MOVE(HyperPeerFilter);

  // Whether a resolved peer address is permitted. `addr` holds the 4 (IPv4) or 16 (IPv6) network-
  // order bytes of the IP; `isIpv6` selects the family; `port` is host-order. Same check, same
  // rules, as kj applies inside connect() (see kj_rs_io::async-io.c++ TokioNetworkAddress).
  bool shouldAllow(bool isIpv6, ::rust::Slice<const uint8_t> addr, uint16_t port) const;

 private:
  // The allow-all root, or the restricted filter chained onto a fresh allow-all root; set once
  // at construction, never rebound. The pointee is non-const (PeerFilter::shouldAllow is
  // non-const), the handle is.
  kj::Rc<kj_rs_io::PeerFilter> active;
  kj_rs_io::PeerFilter& filter() const;
};

// Allow-everything filter (kj's unrestricted root network).
std::unique_ptr<HyperPeerFilter> newAllowAllHyperPeerFilter();

// Filter with the given allow/deny rules (kj::Network::restrictPeers() grammar).
std::unique_ptr<HyperPeerFilter> newHyperPeerFilter(
    kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny);

}  // namespace workerd::rust::kj_hyper
