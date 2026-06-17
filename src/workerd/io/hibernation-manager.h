// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/web-socket.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

// The replacement for LegacyHibernationManagerImpl, introduced behind the
// hibernatable-websocket-refactor autogate as part of EW-10817. Initially a KJ_UNIMPLEMENTED
// skeleton; each override is filled in piecewise as the new path lands. With the autogate off,
// LegacyHibernationManagerImpl is used and production behavior is unchanged.
class HibernationManagerImpl final: public Worker::Actor::HibernationManager {
 public:
  HibernationManagerImpl(kj::Own<Worker::Actor::Loopback> loopback, uint16_t hibernationEventType);
  ~HibernationManagerImpl() noexcept(false);

  void acceptWebSocket(jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) override;
  kj::Vector<jsg::Ref<api::WebSocket>> getWebSockets(
      jsg::Lock& js, kj::Maybe<kj::StringPtr> tag) override;
  void hibernateWebSockets(Worker::Lock& lock) override;
  void setWebSocketAutoResponse(
      kj::Maybe<kj::StringPtr> request, kj::Maybe<kj::StringPtr> response) override;
  kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> getWebSocketAutoResponse(
      jsg::Lock& js) override;
  void setTimerChannel(TimerChannel& timerChannel) override;
  kj::Own<HibernationManager> addRef() override;
  void setEventTimeout(kj::Maybe<uint32_t> timeoutMs) override;
  kj::Maybe<uint32_t> getEventTimeout() override;
};

}  // namespace workerd
