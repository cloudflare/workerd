#pragma once
// restrictPeers() policy for the tokio backend: KJ's own kj::_::NetworkFilter -- the allow/deny
// grammar workerd.capnp documents, unchanged -- behind an atomically refcounted chain, so a
// kj::Network, the addresses parsed from it and the receivers listening on them share one filter
// with no lifetime coupling between them. (KJ's own objects borrow the network's filter by
// reference and rely on the network outliving them.)
//
// Atomic on purpose: KJ allows a kj::NetworkAddress to be cloned on another thread (its state is
// immutable), and clone() takes a share of the filter; kj::Rc's non-atomic count would race. The
// filter itself never changes after construction, so shouldAllow() is safe from any thread.
//
// The adapter (async-io.c++) consults the filter where KJ does: for each target before connect()
// tries it, and for each accepted peer. KJ's parse-time rejection of a filtered *literal*
// (shouldAllowParse) is not reproduced -- connect() rejects the same address a moment later.

#include <kj/async-io-internal.h>
#include <kj/async-io.h>
#include <kj/refcount.h>

namespace kj_rs_io {

class PeerFilter final: public kj::LowLevelAsyncIoProvider::NetworkFilter,
                        public kj::AtomicRefcounted {
 public:
  // The allow-everything root: a kj::Network with no restrictPeers() applied.
  PeerFilter();
  // restrictPeers(allow, deny) over `next`, whose rules stay in force (KJ's chain semantics);
  // `next` is shared so the parent's filter lives as long as any child does.
  PeerFilter(kj::ArrayPtr<const kj::StringPtr> allow,
      kj::ArrayPtr<const kj::StringPtr> deny,
      kj::Arc<PeerFilter> next);
  KJ_DISALLOW_COPY_AND_MOVE(PeerFilter);

  // KJ's decision for `addr`. const: the filter never changes after construction, which is
  // what makes sharing it across threads (kj::Arc) sound; kj::_::NetworkFilter's query is
  // merely not declared const, hence `mutable`.
  bool allows(const struct sockaddr *addr, kj::uint addrlen) const;
  // kj::LowLevelAsyncIoProvider::NetworkFilter.
  bool shouldAllow(const struct sockaddr *addr, kj::uint addrlen) override {
    return allows(addr, addrlen);
  }

 private:
  kj::Arc<PeerFilter> next;  // null for the root
  mutable kj::_::NetworkFilter impl;
};

}  // namespace kj_rs_io
