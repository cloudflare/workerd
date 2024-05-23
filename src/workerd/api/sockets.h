// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>
#include "streams.h"

namespace workerd::api {

class Fetcher;

enum class SecureTransportKind {
  // plain-text
  OFF,
  // plain-text at first, with `startTls` available to upgrade at a later time
  STARTTLS,
  // TLS enabled immediately
  ON,
};

struct SocketAddress {
  kj::String hostname;
  uint16_t port;
  JSG_STRUCT(hostname, port);
};

struct SocketInfo {
  jsg::Optional<kj::String> remoteAddress;
  jsg::Optional<kj::String> localAddress;
  JSG_STRUCT(remoteAddress, localAddress);
};

typedef kj::OneOf<SocketAddress, kj::String> AnySocketAddress;

struct SocketOptions {
  jsg::Optional<kj::String> secureTransport;
  bool allowHalfOpen = false;
  jsg::Optional<uint64_t> highWaterMark;
  JSG_STRUCT(secureTransport, allowHalfOpen, highWaterMark);
  JSG_MEMORY_INFO(SocketOptions) {
    tracker.trackField("secureTransport", secureTransport);
  }
};

struct TlsOptions {
  jsg::Optional<kj::String> expectedServerHostname;
  JSG_STRUCT(expectedServerHostname);
};

class Socket: public jsg::Object {
public:
  Socket(jsg::Lock& js, IoContext& context,
      kj::Own<kj::RefcountedWrapper<kj::Own<kj::AsyncIoStream>>> connectionStream,
      jsg::Ref<ReadableStream> readableParam, jsg::Ref<WritableStream> writable,
      jsg::PromiseResolverPair<void> closedPrPair, kj::Promise<void> watchForDisconnectTask,
      jsg::Optional<SocketOptions> options, kj::Own<kj::TlsStarterCallback> tlsStarter,
      bool isSecureSocket, kj::String domain, bool isDefaultFetchPort,
      jsg::PromiseResolverPair<SocketInfo> openedPrPair)
      : connectionStream(context.addObject(kj::mv(connectionStream))),
        readable(kj::mv(readableParam)), writable(kj::mv(writable)),
        closedResolver(kj::mv(closedPrPair.resolver)),
        closedPromiseCopy(closedPrPair.promise.whenResolved(js)),
        closedPromise(kj::mv(closedPrPair.promise)),
        watchForDisconnectTask(context.addObject(kj::heap(kj::mv(watchForDisconnectTask)))),
        options(kj::mv(options)),
        tlsStarter(context.addObject(kj::mv(tlsStarter))),
        isSecureSocket(isSecureSocket),
        domain(kj::mv(domain)),
        isDefaultFetchPort(isDefaultFetchPort),
        openedResolver(kj::mv(openedPrPair.resolver)),
        openedPromiseCopy(openedPrPair.promise.whenResolved(js)),
        openedPromise(kj::mv(openedPrPair.promise)),
        isClosing(false) { };

  jsg::Ref<ReadableStream> getReadable() { return readable.addRef(); }
  jsg::Ref<WritableStream> getWritable() { return writable.addRef(); }
  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed() {
    return closedPromise;
  }
  jsg::MemoizedIdentity<jsg::Promise<SocketInfo>>& getOpened() {
    return openedPromise;
  }

  // Closes the socket connection.
  //
  // The closure is only performed after the socket connection is properly
  // established through any configured proxy. This method also flushes the writable stream prior to
  // closing.
  jsg::Promise<void> close(jsg::Lock& js);

  // Flushes write buffers then performs a TLS handshake on the current Socket connection.
  // The current `Socket` instance is closed and its readable/writable instances are also closed.
  // All new operations should be performed on the new `Socket` instance.
  jsg::Ref<Socket> startTls(jsg::Lock& js, jsg::Optional<TlsOptions> options);

  // Sets up relevant callbacks to handle the case when the proxy rejects our connection.
  // The first variant is useful for connections established using HTTP connect. The latter is for
  // connections established any other way, where the lack of an exception indicates we connected
  // successfully.
  void handleProxyStatus(
      jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status);

  // Sets up relevant callbacks to handle the case when the proxy rejects our connection.
  // The first variant is useful for connections established using HTTP connect. The latter is for
  // connections established any other way, where the lack of an exception indicates we connected
  // successfully.
  void handleProxyStatus(jsg::Lock& js, kj::Promise<kj::Maybe<kj::Exception>> status);

  void handleReadableEof(jsg::Lock& js, jsg::Promise<void> onEof);
  // Sets up relevant callbacks to handle the case when the readable stream reaches EOF.

  JSG_RESOURCE_TYPE(Socket) {
    JSG_READONLY_PROTOTYPE_PROPERTY(readable, getReadable);
    JSG_READONLY_PROTOTYPE_PROPERTY(writable, getWritable);
    JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
    JSG_READONLY_PROTOTYPE_PROPERTY(opened, getOpened);
    JSG_METHOD(close);
    JSG_METHOD(startTls);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("connectionStream",
        sizeof(IoOwn<kj::RefcountedWrapper<kj::Own<kj::AsyncIoStream>>>));
    tracker.trackFieldWithSize("tlsStarter",
        sizeof(IoOwn<kj::TlsStarterCallback>));
    tracker.trackFieldWithSize("watchForDisconnectTask",
        sizeof(IoOwn<kj::Promise<void>>));
    tracker.trackField("readable", readable);
    tracker.trackField("writable", writable);
    tracker.trackField("closedResolver", closedResolver);
    tracker.trackField("closedPromiseCopy", closedPromiseCopy);
    tracker.trackField("closedPromise", closedPromise);
    tracker.trackField("options", options);
    tracker.trackField("domain", domain);
    tracker.trackField("openedResolver", openedResolver);
    tracker.trackField("openedPromiseCopy", openedPromiseCopy);
    tracker.trackField("openedPromise", openedPromise);
  }

private:
  // TODO(cleanup): Combine all the IoOwns here into one, to improve efficiency and make
  //   shutdown order clearer.

  IoOwn<kj::RefcountedWrapper<kj::Own<kj::AsyncIoStream>>> connectionStream;
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  // This fulfiller is used to resolve the `closedPromise` below.
  jsg::Promise<void>::Resolver closedResolver;
  // Copy kept so that it can be returned from `close`.
  jsg::Promise<void> closedPromiseCopy;
  // Memoized copy that is returned by the `closed` attribute.
  jsg::MemoizedIdentity<jsg::Promise<void>> closedPromise;
  IoOwn<kj::Promise<void>> watchForDisconnectTask;
  jsg::Optional<SocketOptions> options;
  // Callback used to upgrade the existing connection to a secure one.
  IoOwn<kj::TlsStarterCallback> tlsStarter;
  // Set to true on sockets created with `useSecureTransport` set to true or a socket returned by
  // `startTls`.
  bool isSecureSocket;
  // The domain/ip this socket is connected to. Used for startTls.
  kj::String domain;
  // Whether the port this socket connected to is 80/443. Used for nicer errors.
  bool isDefaultFetchPort;
  // This fulfiller is used to resolve the `openedPromise` below.
  jsg::Promise<SocketInfo>::Resolver openedResolver;
  // Copy kept so that it can be used in `close`.
  jsg::Promise<void> openedPromiseCopy;
  jsg::MemoizedIdentity<jsg::Promise<SocketInfo>> openedPromise;
  // Used to keep track of a pending `close` operation on the socket.
  bool isClosing;

  kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection();
  jsg::Promise<void> maybeCloseWriteSide(jsg::Lock& js);
  jsg::Promise<void> closeImplOld(jsg::Lock& js);
  jsg::Promise<void> closeImplNew(jsg::Lock& js);

  // Helper method for handleProxyStatus implementations.
  void handleProxyError(jsg::Lock& js, kj::Exception e);

  void resolveFulfiller(jsg::Lock& js, kj::Maybe<kj::Exception> maybeErr) {
    KJ_IF_SOME(err, maybeErr) {
      closedResolver.reject(js, kj::cp(err));
    } else {
      closedResolver.resolve(js);
    }
  };

  void errorHandler(jsg::Lock& js, jsg::Value err) {
    auto jsException = err.getHandle(js);
    resolveFulfiller(js, jsg::createTunneledException(js.v8Isolate, jsException));
  };

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(readable, writable, closedResolver,
                  closedPromiseCopy, closedPromise,
                  openedResolver, openedPromiseCopy,
                  openedPromise);
  }
};

jsg::Ref<Socket> setupSocket(
    jsg::Lock& js, kj::Own<kj::AsyncIoStream> connection,
    jsg::Optional<SocketOptions> options, kj::Own<kj::TlsStarterCallback> tlsStarter,
    bool isSecureSocket, kj::String domain, bool isDefaultFetchPort);

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options);

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options);

class SocketsModule final: public jsg::Object {
public:
  jsg::Ref<Socket> connect(jsg::Lock& js, AnySocketAddress address,
    jsg::Optional<SocketOptions> options) {
    return connectImpl(js, kj::none, kj::mv(address), kj::mv(options));
  }

  JSG_RESOURCE_TYPE(SocketsModule) {
    JSG_METHOD(connect);
  }
};

template <class Registry>
void registerSocketsModule(
    Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<SocketsModule>("cloudflare-internal:sockets",
    workerd::jsg::ModuleRegistry::Type::INTERNAL);

}

#define EW_SOCKETS_ISOLATE_TYPES     \
  api::Socket,                       \
  api::SocketOptions,                \
  api::SocketAddress,                \
  api::TlsOptions,                   \
  api::SocketsModule,                \
  api::SocketInfo

// The list of sockets.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
