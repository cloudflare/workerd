// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async-io.h>
#include <kj/function.h>

namespace workerd::api {

// A request from whoever is on the far side of a stream to upgrade that stream to TLS, in a form
// that someone other than the stream's owner can answer. Socket::proxyTo() answers it by upgrading
// the stream it relays to, so the request travels on to wherever the handshake can be made.
class InboundTlsUpgrade {
 public:
  virtual ~InboundTlsUpgrade() noexcept(false) = default;

  // Resolves when the far side asks for the upgrade. The request is latched, so this also
  // resolves if the far side asked before anyone was listening. Rejects if the far side goes away
  // without asking.
  virtual kj::Promise<void> whenRequested() = 0;

  // Answers the far side's request: the upgrade happened on its behalf if `failure` is none, and
  // did not otherwise, in which case the far side sees `failure`. Only the first answer counts;
  // later ones are ignored.
  virtual void answer(kj::Maybe<kj::Exception> failure) = 0;
};

// One of the two streams a relay joins, and what can happen to its transport.
struct RelayEnd {
  kj::Own<kj::AsyncIoStream> stream;

  // Upgrade requests arriving from the far side of `stream`, if it can make them.
  kj::Maybe<kj::Own<InboundTlsUpgrade>> inboundUpgrade;

  // Upgrades the transport underneath `stream` to TLS, if it can be. The stream keeps being used
  // after the returned promise resolves, and carries plaintext either side of the upgrade.
  //
  // Declared after `stream` so that it is destroyed first: it may refer to the transport.
  kj::Maybe<kj::Function<kj::Promise<void>()>> startTls;
};

// Copies bytes between `a` and `b` in both directions until both directions have ended. Each
// direction ends independently: when one stream reaches EOF, the other has its write side shut
// down and the opposite direction carries on. Failure in either direction fails the whole relay.
//
// When the far side of one end asks for a TLS upgrade, the relay makes the upgrade on the other
// end instead, and answers the request with the outcome. Every byte that the far side sent before
// asking is written to the other end before its startTls() is called, and nothing that follows
// is written until that has resolved. A request that cannot be honored, because the other end
// cannot be upgraded or the upgrade fails, fails the relay: carrying on in plaintext after an
// upgrade was asked for would be a silent downgrade.
kj::Promise<void> relayStreams(RelayEnd a, RelayEnd b);

}  // namespace workerd::api
