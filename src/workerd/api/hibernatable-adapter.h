// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/web-socket.h>

namespace workerd::api {

// HibernatableWebSocketAdapter is the EW-10817 replacement for
// `LegacyWebSocketAdapter` on hibernatable WebSockets. It is selected at
// `acceptAsHibernatable` time (and at hibernation revival) when the
// `hibernatable-websocket-refactor` autogate is enabled.
//
// This skeleton lands the class shape and the autogate consultation points; every method
// throws `KJ_UNIMPLEMENTED` and is filled in piecewise in subsequent commits.
class HibernatableWebSocketAdapter final: public WebSocketAdapter {
 public:
  // Revival constructor — chosen by the shell when waking a hibernated WebSocket if the
  // `hibernatable-websocket-refactor` autogate is enabled.
  HibernatableWebSocketAdapter(jsg::Lock& js,
      WebSocket& shell,
      IoContext& ioContext,
      kj::WebSocket& ws,
      WebSocket::HibernationPackage package);

  // Transition constructor — invoked from `WebSocket::acceptAsHibernatable` when the
  // autogate is on, after the kj::WebSocket has been extracted from the legacy adapter.
  // Receives the surviving identity from the legacy adapter together with a reference to
  // the kj::WebSocket (which is being transferred to the HibernationManager and lives there
  // for the rest of the connection).
  //
  // Does not take a `jsg::Lock&` because the call site in the shell does not have one
  // available (`HibernationManager::acceptWebSocket` does not currently plumb one through).
  // When the implementation actually needs JS state, it can grab the lock from the ambient
  // IoContext.
  HibernatableWebSocketAdapter(WebSocket& shell, kj::WebSocket& ws, kj::Array<kj::StringPtr> tags);

  ~HibernatableWebSocketAdapter() noexcept(false);

  // ---------------------------------------------------------------------------
  // WebSocketAdapter overrides — see WebSocketAdapter for full method docs. All throw
  // `KJ_UNIMPLEMENTED` until the corresponding piece of the new path lands.
  // ---------------------------------------------------------------------------

  void accept(jsg::Lock& js, jsg::Optional<WebSocket::AcceptOptions> options) override;
  void send(jsg::Lock& js, kj::OneOf<kj::Array<byte>, kj::String> message) override;
  void close(jsg::Lock& js, jsg::Optional<int> code, jsg::Optional<jsg::USVString> reason) override;
  void serializeAttachment(jsg::Lock& js, jsg::JsValue attachment) override;
  kj::Maybe<jsg::JsValue> deserializeAttachment(jsg::Lock& js) override;
  int getReadyState() override;
  kj::Maybe<kj::StringPtr> getUrl() override;
  kj::Maybe<kj::StringPtr> getProtocol() override;
  kj::Maybe<kj::StringPtr> getExtensions() override;
  kj::StringPtr getBinaryType() override;
  void setBinaryType(kj::String value) override;

  void initConnection(jsg::Lock& js, kj::Promise<PackedWebSocket> packedWsPromise) override;
  kj::Promise<DeferredProxy<void>> couple(
      jsg::Lock& js, kj::Own<kj::WebSocket> other, RequestObserver& request) override;
  void internalAccept(jsg::Lock& js, kj::Maybe<kj::Own<InputGate::CriticalSection>> cs) override;

  bool isAccepted() override;
  bool isReleased() override;
  bool isAwaitingCoupling() override;
  bool isHibernatable() override;
  void setObserver(kj::Own<WebSocketObserver> observer) override;

  kj::Own<kj::WebSocket> acceptAsHibernatable(kj::Array<kj::StringPtr> tags) override;
  void initiateHibernatableRelease(jsg::Lock& js,
      kj::Own<kj::WebSocket> ws,
      kj::Array<kj::String> tags,
      WebSocket::HibernatableReleaseState releaseState) override;
  bool awaitingHibernatableError() override;
  bool awaitingHibernatableRelease() override;

  WebSocket::HibernationPackage buildPackageForHibernation() override;
  kj::Array<kj::StringPtr> getHibernatableTags() override;
  kj::Maybe<kj::String> getPreferredExtensions(kj::WebSocket::ExtensionsContext ctx) override;

  void setAutoResponseStatus(
      kj::Maybe<kj::Date> time, kj::Promise<void> autoResponsePromise) override;
  kj::Maybe<kj::Date> getAutoResponseTimestamp() override;
  kj::Promise<void> sendAutoResponse(kj::String message, kj::WebSocket& ws) override;

  void setPeer(jsg::WeakRef<WebSocket> peer) override;
  bool peerIsAwaitingCoupling(jsg::Lock& js) override;

  void visitForGc(jsg::GcVisitor& visitor) override;
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const override;

 private:
  // Back-reference to the JSG-visible shell, used for event dispatch (analogous to
  // `LegacyWebSocketAdapter::shell`). `[[maybe_unused]]` suppresses the diagnostic while
  // every override is `KJ_UNIMPLEMENTED`; the attribute can be removed once the first method
  // that fires events through the shell lands.
  [[maybe_unused]] WebSocket& shell;
};

}  // namespace workerd::api
