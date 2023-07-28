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
  OFF, // plain-text
  STARTTLS, // plain-text at first, with `startTls` available to upgrade at a later time
  ON // TLS enabled immediately
};

struct SocketAddress {
  kj::String hostname;
  uint16_t port;
  JSG_STRUCT(hostname, port);
};

typedef kj::OneOf<SocketAddress, kj::String> AnySocketAddress;

struct SocketOptions {
  jsg::Optional<kj::String> secureTransport;
  bool allowHalfOpen = false;
  JSG_STRUCT(secureTransport, allowHalfOpen);
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
      bool isSecureSocket, kj::String domain, bool isDefaultFetchPort)
      : connectionStream(context.addObject(kj::mv(connectionStream))),
        readable(kj::mv(readableParam)), writable(kj::mv(writable)),
        closedResolver(kj::mv(closedPrPair.resolver)),
        closedPromise(kj::mv(closedPrPair.promise)),
        watchForDisconnectTask(context.addObject(kj::heap(kj::mv(watchForDisconnectTask)))),
        options(kj::mv(options)),
        tlsStarter(context.addObject(kj::mv(tlsStarter))),
        isSecureSocket(isSecureSocket),
        domain(kj::mv(domain)),
        isDefaultFetchPort(isDefaultFetchPort) { };

  jsg::Ref<ReadableStream> getReadable() { return readable.addRef(); }
  jsg::Ref<WritableStream> getWritable() { return writable.addRef(); }
  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed() {
    return closedPromise;
  }

  jsg::Promise<void> close(jsg::Lock& js);
  // Closes the socket connection.

  jsg::Ref<Socket> startTls(jsg::Lock& js, jsg::Optional<TlsOptions> options);
  // Flushes write buffers then performs a TLS handshake on the current Socket connection.
  // The current `Socket` instance is closed and its readable/writable instances are also closed.
  // All new operations should be performed on the new `Socket` instance.

  void handleProxyStatus(
      jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status);
  // Sets up relevant callbacks to handle the case when the proxy rejects our connection.

  void handleReadableEof(jsg::Lock& js, jsg::Promise<void> onEof);
  // Sets up relevant callbacks to handle the case when the readable stream reaches EOF.

  JSG_RESOURCE_TYPE(Socket) {
    JSG_READONLY_PROTOTYPE_PROPERTY(readable, getReadable);
    JSG_READONLY_PROTOTYPE_PROPERTY(writable, getWritable);
    JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
    JSG_METHOD(close);
    JSG_METHOD(startTls);
  }

private:
  // TODO(cleanup): Combine all the IoOwns here into one, to improve efficiency and make
  //   shutdown order clearer.

  IoOwn<kj::RefcountedWrapper<kj::Own<kj::AsyncIoStream>>> connectionStream;
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  jsg::Promise<void>::Resolver closedResolver;
  // This fulfiller is used to resolve the `closedPromise` below.
  jsg::MemoizedIdentity<jsg::Promise<void>> closedPromise;
  IoOwn<kj::Promise<void>> watchForDisconnectTask;
  jsg::Optional<SocketOptions> options;
  IoOwn<kj::TlsStarterCallback> tlsStarter;
  // Callback used to upgrade the existing connection to a secure one.
  bool isSecureSocket;
  // Set to true on sockets created with `useSecureTransport` set to true or a socket returned by
  // `startTls`.
  kj::String domain;
  // The domain/ip this socket is connected to. Used for startTls.
  bool isDefaultFetchPort;
  // Whether the port this socket connected to is 80/443. Used for nicer errors.

  kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection();
  jsg::Promise<void> maybeCloseWriteSide(jsg::Lock& js);

  void resolveFulfiller(jsg::Lock& js, kj::Maybe<kj::Exception> maybeErr) {
    KJ_IF_MAYBE(err, maybeErr) {
      closedResolver.reject(js, kj::cp(*err));
    } else {
      closedResolver.resolve(js);
    }
  };

  jsg::Promise<void> errorHandler(jsg::Lock& js, jsg::Value err) {
    auto jsException = err.getHandle(js.v8Isolate);
    resolveFulfiller(js, jsg::createTunneledException(js.v8Isolate, jsException));
    return js.resolvedPromise();
  };

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(readable, writable);
  }
};

jsg::Ref<Socket> setupSocket(
    jsg::Lock& js, kj::Own<kj::AsyncIoStream> connection,
    jsg::Optional<SocketOptions> options, kj::Own<kj::TlsStarterCallback> tlsStarter,
    bool isSecureSocket, kj::String domain, bool isDefaultFetchPort);

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options);

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options);

class SocketsModule final: public jsg::Object {
public:
  jsg::Ref<Socket> connect(jsg::Lock& js, AnySocketAddress address,
    jsg::Optional<SocketOptions> options) {
    return connectImpl(js, nullptr, kj::mv(address), kj::mv(options));
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
  api::SocketsModule

// The list of sockets.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
