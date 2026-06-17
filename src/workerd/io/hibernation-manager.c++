// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hibernation-manager.h"

namespace workerd {

HibernationManagerImpl::HibernationManagerImpl(
    kj::Own<Worker::Actor::Loopback> loopback, uint16_t hibernationEventType) {
  // Constructor body intentionally left empty; the new implementation lands piecewise behind
  // the hibernatable-websocket-refactor autogate (EW-10817). With the autogate off,
  // LegacyHibernationManagerImpl is constructed instead and this class is never instantiated.
  KJ_UNIMPLEMENTED("new HibernationManagerImpl not yet implemented (EW-10817)");
}

HibernationManagerImpl::~HibernationManagerImpl() noexcept(false) {}

void HibernationManagerImpl::acceptWebSocket(
    jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) {
  KJ_UNIMPLEMENTED("HibernationManagerImpl::acceptWebSocket not yet implemented (EW-10817)");
}

kj::Vector<jsg::Ref<api::WebSocket>> HibernationManagerImpl::getWebSockets(
    jsg::Lock& js, kj::Maybe<kj::StringPtr> tag) {
  KJ_UNIMPLEMENTED("HibernationManagerImpl::getWebSockets not yet implemented (EW-10817)");
}

void HibernationManagerImpl::hibernateWebSockets(Worker::Lock& lock) {
  KJ_UNIMPLEMENTED("HibernationManagerImpl::hibernateWebSockets not yet implemented (EW-10817)");
}

void HibernationManagerImpl::setWebSocketAutoResponse(
    kj::Maybe<kj::StringPtr> request, kj::Maybe<kj::StringPtr> response) {
  KJ_UNIMPLEMENTED(
      "HibernationManagerImpl::setWebSocketAutoResponse not yet implemented (EW-10817)");
}

kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> HibernationManagerImpl::
    getWebSocketAutoResponse(jsg::Lock& js) {
  KJ_UNIMPLEMENTED(
      "HibernationManagerImpl::getWebSocketAutoResponse not yet implemented (EW-10817)");
}

void HibernationManagerImpl::setTimerChannel(TimerChannel& timerChannel) {
  KJ_UNIMPLEMENTED("HibernationManagerImpl::setTimerChannel not yet implemented (EW-10817)");
}

kj::Own<Worker::Actor::HibernationManager> HibernationManagerImpl::addRef() {
  KJ_UNIMPLEMENTED("HibernationManagerImpl::addRef not yet implemented (EW-10817)");
}

void HibernationManagerImpl::setEventTimeout(kj::Maybe<uint32_t> timeoutMs) {
  KJ_UNIMPLEMENTED("HibernationManagerImpl::setEventTimeout not yet implemented (EW-10817)");
}

kj::Maybe<uint32_t> HibernationManagerImpl::getEventTimeout() {
  KJ_UNIMPLEMENTED("HibernationManagerImpl::getEventTimeout not yet implemented (EW-10817)");
}

}  // namespace workerd
