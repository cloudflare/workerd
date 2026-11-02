// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/async.h>

namespace workerd {

class DatagramChannel {
 public:
  virtual ~DatagramChannel() noexcept(false) = default;
  // Closing an owning channel cancels pending operations and releases its transport.
  // Borrowed ingress channels are ended by their listener instead.
  virtual void close() {}

  // One datagram, or none for flow EOF. Only one receive may be pending at a time.
  virtual kj::Promise<kj::Maybe<kj::Array<kj::byte>>> receive() = 0;
  // One send is exactly one packet, including an empty array. Caller keeps bytes alive
  // until completion. Success is local acceptance, never acknowledgement by the peer.
  virtual kj::Promise<void> send(kj::ArrayPtr<const kj::byte> datagram) = 0;
};

}  // namespace workerd
