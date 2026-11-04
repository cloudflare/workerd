// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/observer.h>

#include <kj/refcount.h>

namespace workerd {

// The transport currently carrying a hibernatable WebSocket to its client, and how many of the
// messages it sent the socket has received. The HibernationManager keeps one per socket. Whatever
// connects the socket to a transport attaches it: couple(), for the request whose response carries
// the socket's client end, or an embedder that reconnects an adopted socket.
class HibernatableTransport {
 public:
  // Makes `handle` the socket's transport, replacing any previous one, and restarts the count of
  // received messages, which is compared against the count the transport reports when paused.
  // `handle` is kj::none if what connected the socket supplied none. The socket then receives in a
  // way that a handoff would interrupt, so from then on it can't be handed off, and it keeps no
  // handle that a later transport supplies.
  void attach(kj::Maybe<kj::Own<HibernatableTransportHandle>> handle) {
    if (handle == kj::none) {
      attachedWithoutHandle = true;
    }
    if (attachedWithoutHandle) {
      this->handle = kj::none;
    } else {
      this->handle = kj::mv(handle);
    }
    consumedCount = 0;
  }

  kj::Maybe<HibernatableTransportHandle&> getHandle() {
    return handle;
  }

  // False once the socket was connected to a transport without a handle. Until the socket is
  // connected, it may yet get one.
  bool mayHaveHandle() const {
    return !attachedWithoutHandle;
  }

  // Messages, including Close, that the socket has received since its transport was attached.
  uint64_t getConsumedCount() const {
    return consumedCount;
  }

  void countConsumed() {
    ++consumedCount;
  }

 private:
  kj::Maybe<kj::Own<HibernatableTransportHandle>> handle;
  bool attachedWithoutHandle = false;
  uint64_t consumedCount = 0;
};

}  // namespace workerd
