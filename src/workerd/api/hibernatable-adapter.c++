// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hibernatable-adapter.h"

#include <workerd/io/io-context.h>

namespace workerd::api {

HibernatableWebSocketAdapter::HibernatableWebSocketAdapter(jsg::Lock& js,
    WebSocket& shellParam,
    IoContext& ioContext,
    kj::WebSocket& ws,
    WebSocket::HibernationPackage package)
    : shell(shellParam) {
  KJ_UNIMPLEMENTED("EW-10817: HibernatableWebSocketAdapter revival ctor not yet implemented");
}

HibernatableWebSocketAdapter::HibernatableWebSocketAdapter(
    WebSocket& shellParam, kj::WebSocket& ws, kj::Array<kj::StringPtr> tags)
    : shell(shellParam) {
  KJ_UNIMPLEMENTED("EW-10817: HibernatableWebSocketAdapter transition ctor not yet implemented");
}

HibernatableWebSocketAdapter::~HibernatableWebSocketAdapter() noexcept(false) {}

namespace {
[[noreturn]] void unimplemented() {
  KJ_UNIMPLEMENTED("EW-10817: HibernatableWebSocketAdapter method not yet implemented");
}
}  // namespace

void HibernatableWebSocketAdapter::accept(jsg::Lock&, jsg::Optional<WebSocket::AcceptOptions>) {
  unimplemented();
}

void HibernatableWebSocketAdapter::send(jsg::Lock&, kj::OneOf<kj::Array<byte>, kj::String>) {
  unimplemented();
}

void HibernatableWebSocketAdapter::close(
    jsg::Lock&, jsg::Optional<int>, jsg::Optional<jsg::USVString>) {
  unimplemented();
}

void HibernatableWebSocketAdapter::serializeAttachment(jsg::Lock&, jsg::JsValue) {
  unimplemented();
}

kj::Maybe<jsg::JsValue> HibernatableWebSocketAdapter::deserializeAttachment(jsg::Lock&) {
  unimplemented();
}

int HibernatableWebSocketAdapter::getReadyState() {
  unimplemented();
}

kj::Maybe<kj::StringPtr> HibernatableWebSocketAdapter::getUrl() {
  unimplemented();
}

kj::Maybe<kj::StringPtr> HibernatableWebSocketAdapter::getProtocol() {
  unimplemented();
}

kj::Maybe<kj::StringPtr> HibernatableWebSocketAdapter::getExtensions() {
  unimplemented();
}

kj::StringPtr HibernatableWebSocketAdapter::getBinaryType() {
  unimplemented();
}

void HibernatableWebSocketAdapter::setBinaryType(kj::String) {
  unimplemented();
}

void HibernatableWebSocketAdapter::initConnection(jsg::Lock&, kj::Promise<PackedWebSocket>) {
  unimplemented();
}

kj::Promise<DeferredProxy<void>> HibernatableWebSocketAdapter::couple(
    jsg::Lock&, kj::Own<kj::WebSocket>, RequestObserver&) {
  unimplemented();
}

void HibernatableWebSocketAdapter::internalAccept(
    jsg::Lock&, kj::Maybe<kj::Own<InputGate::CriticalSection>>) {
  unimplemented();
}

bool HibernatableWebSocketAdapter::isHibernatable() {
  // The HibernatableWebSocketAdapter is selected for hibernatable WebSockets by definition.
  return true;
}

bool HibernatableWebSocketAdapter::isAccepted() {
  unimplemented();
}

bool HibernatableWebSocketAdapter::isReleased() {
  unimplemented();
}

bool HibernatableWebSocketAdapter::isAwaitingCoupling() {
  // The HibernatableWebSocketAdapter is only selected post-acceptAsHibernatable / on revival
  // from hibernation, both of which are post-accept states. Never awaiting coupling.
  return false;
}

kj::Own<kj::WebSocket> HibernatableWebSocketAdapter::acceptAsHibernatable(
    kj::Array<kj::StringPtr>) {
  unimplemented();
}

void HibernatableWebSocketAdapter::initiateHibernatableRelease(jsg::Lock&,
    kj::Own<kj::WebSocket>,
    kj::Array<kj::String>,
    WebSocket::HibernatableReleaseState) {
  unimplemented();
}

bool HibernatableWebSocketAdapter::awaitingHibernatableError() {
  unimplemented();
}

bool HibernatableWebSocketAdapter::awaitingHibernatableRelease() {
  unimplemented();
}

WebSocket::HibernationPackage HibernatableWebSocketAdapter::buildPackageForHibernation() {
  unimplemented();
}

kj::Array<kj::StringPtr> HibernatableWebSocketAdapter::getHibernatableTags() {
  unimplemented();
}

kj::Maybe<kj::String> HibernatableWebSocketAdapter::getPreferredExtensions(
    kj::WebSocket::ExtensionsContext) {
  unimplemented();
}

void HibernatableWebSocketAdapter::setAutoResponseStatus(kj::Maybe<kj::Date>, kj::Promise<void>) {
  unimplemented();
}

kj::Maybe<kj::Date> HibernatableWebSocketAdapter::getAutoResponseTimestamp() {
  unimplemented();
}

kj::Promise<void> HibernatableWebSocketAdapter::sendAutoResponse(kj::String, kj::WebSocket&) {
  unimplemented();
}

void HibernatableWebSocketAdapter::setPeer(jsg::WeakRef<WebSocket>) {
  unimplemented();
}

bool HibernatableWebSocketAdapter::peerIsAwaitingCoupling(jsg::Lock&) {
  unimplemented();
}

void HibernatableWebSocketAdapter::setObserver(kj::Own<WebSocketObserver>) {
  unimplemented();
}

void HibernatableWebSocketAdapter::visitForGc(jsg::GcVisitor&) {
  // No-op until the new adapter holds GC-traced state.
}

void HibernatableWebSocketAdapter::visitForMemoryInfo(jsg::MemoryTracker&) const {
  // No-op until the new adapter holds memory-tracked state.
}

}  // namespace workerd::api
