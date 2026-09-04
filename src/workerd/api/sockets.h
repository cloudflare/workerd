// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-writable-stream.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

#include <kj/refcount.h>

namespace workerd {

// A single logical UDP flow between a peer and this side, used to deliver datagram-shaped
// connections to a Worker's connect() handler. Unlike kj::AsyncIoStream, this interface has no
// byte-stream semantics: each receive() resolves to exactly one datagram (or none, once the flow
// has ended), and each send() transmits exactly one, so callers cannot accidentally merge or split
// datagrams the way partial tryRead()/write() calls would allow.
class DatagramChannel {
 public:
  virtual ~DatagramChannel() noexcept(false) = default;

  // Resolves with the next inbound datagram, or kj::none once the flow has ended (e.g. an idle
  // timeout). Must not be called again after resolving kj::none, and must not have more than one
  // outstanding call at a time.
  virtual kj::Promise<kj::Maybe<kj::Array<kj::byte>>> receive() = 0;

  // Sends one outbound datagram to the peer.
  virtual kj::Promise<void> send(kj::ArrayPtr<const kj::byte> datagram) = 0;
};

// A DatagramChannel wrapper that can be disconnected, mirroring
// workerd::NeuterableIoStream (src/workerd/util/stream-utils.h). Used when a DatagramChannel is
// borrowed by reference for the duration of a single dispatch (see
// ServiceWorkerGlobalScope::connectUdp()): the wrapper forwards to the real channel until
// neuter()'d, at which point further calls fail cleanly instead of touching a reference that may
// no longer be meaningful to use (e.g. after the dispatch's own promise has settled).
class NeuterableDatagramChannel: public DatagramChannel, public kj::Refcounted {
 public:
  virtual void neuter(kj::Exception ex) = 0;
};

kj::Rc<NeuterableDatagramChannel> newNeuterableDatagramChannel(DatagramChannel&);

}  // namespace workerd

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

enum class SocketProtocol {
  // A byte-stream socket (TCP).
  TCP,
  // A datagram socket (UDP).
  UDP,
};

struct SocketAddress {
  kj::String hostname;
  uint16_t port;
  JSG_STRUCT(hostname, port);
};

struct SocketInfo {
  jsg::Optional<kj::String> remoteAddress;

  // The local address — i.e. the address on this side of the socket. For outbound sockets created
  // via `connect()`, we don't have a useful value to provide and leave it empty. For inbound
  // sockets delivered to a worker's `connect(socket)` handler, this is populated with the CONNECT
  // authority (the "host:port" string the caller passed to `fetcher.connect(...)`), since from the
  // handler's perspective that is the address the peer asked to connect to on this end.
  jsg::Optional<kj::String> localAddress;
  JSG_STRUCT(remoteAddress, localAddress);
};

using AnySocketAddress = kj::OneOf<SocketAddress, kj::String>;

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

// A non-standard extension: not part of the proposed sockets spec
// (https://sockets-api.proposal.wintertc.org/), which only covers TCP.
//
// The value-mode chunk type carried by a UDP Socket's readable/writable streams. Deliberately
// not a Uint8Array to avoid consumers treating a UDP stream as a byte stream.
class Datagram: public jsg::Object {
 public:
  Datagram(jsg::Lock& js, jsg::JsUint8Array data): data(js, data) {}

  jsg::JsUint8Array getData(jsg::Lock& js) const {
    return data.getHandle(js);
  }

  static jsg::Ref<Datagram> constructor(jsg::Lock& js, jsg::JsUint8Array data) {
    return js.alloc<Datagram>(js, data);
  }

  JSG_RESOURCE_TYPE(Datagram) {
    JSG_READONLY_PROTOTYPE_PROPERTY(data, getData);
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(data);
  }

 private:
  jsg::JsRef<jsg::JsUint8Array> data;
};

class Socket: public jsg::Object {
 public:
  Socket(jsg::Lock& js,
      IoContext& context,
      kj::OneOf<kj::Rc<kj::AsyncIoStream>, kj::Rc<DatagramChannel>> connectionStream,
      kj::Maybe<kj::String> remoteAddress,
      kj::Maybe<kj::String> localAddress,
      JsReadableStream readableParam,
      JsWritableStream writable,
      jsg::PromiseResolverPair<void> closedPrPair,
      kj::Promise<void> watchForDisconnectTask,
      jsg::Optional<SocketOptions> options,
      kj::Own<kj::TlsStarterCallback> tlsStarter,
      SecureTransportKind secureTransport,
      SocketProtocol protocol,
      kj::Maybe<kj::String> domain,
      bool isDefaultFetchPort,
      jsg::PromiseResolverPair<SocketInfo> openedPrPair)
      : connectionData(context.createObject<ConnectionData>(
            kj::mv(tlsStarter), kj::mv(connectionStream), kj::mv(watchForDisconnectTask))),
        readable(kj::mv(readableParam)),
        writable(kj::mv(writable)),
        closedResolver(kj::mv(closedPrPair.resolver)),
        closedPromiseCopy(closedPrPair.promise.whenResolved(js)),
        closedPromise(kj::mv(closedPrPair.promise)),
        options(kj::mv(options)),
        remoteAddress(kj::mv(remoteAddress)),
        localAddress(kj::mv(localAddress)),
        secureTransport(secureTransport),
        protocol(protocol),
        domain(kj::mv(domain)),
        isDefaultFetchPort(isDefaultFetchPort),
        openedResolver(kj::mv(openedPrPair.resolver)),
        openedPromiseCopy(openedPrPair.promise.whenResolved(js)),
        openedPromise(kj::mv(openedPrPair.promise)) {};

  JsReadableStream getReadable(jsg::Lock& js) {
    return readable.addRef(js);
  }
  JsWritableStream getWritable(jsg::Lock& js) {
    return writable.addRef(js);
  }
  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed() {
    return closedPromise;
  }
  jsg::MemoizedIdentity<jsg::Promise<SocketInfo>>& getOpened() {
    return openedPromise;
  }

  bool getUpgraded() const {
    return upgraded;
  }

  kj::StringPtr getSecureTransport() const {
    switch (secureTransport) {
      case SecureTransportKind::OFF:
        return "off"_kj;
      case SecureTransportKind::STARTTLS:
        return "starttls"_kj;
      case SecureTransportKind::ON:
        return "on"_kj;
    }
  }

  kj::StringPtr getProtocol() const {
    switch (protocol) {
      case SocketProtocol::TCP:
        return "tcp"_kj;
      case SocketProtocol::UDP:
        return "udp"_kj;
    }
    KJ_UNREACHABLE;
  }

  // Takes ownership of the underlying connection stream, detaching the readable and writable streams.
  // This is a destructive operation that renders the Socket unusable for further I/O operations.
  kj::Own<kj::AsyncIoStream> takeConnectionStream(jsg::Lock& js);

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
  void handleProxyStatus(jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status);

  // Sets up relevant callbacks to handle the case when the proxy rejects our connection.
  // The first variant is useful for connections established using HTTP connect. The latter is for
  // connections established any other way, where the lack of an exception indicates we connected
  // successfully.
  void handleProxyStatus(jsg::Lock& js, kj::Promise<kj::Maybe<kj::Exception>> status);

  void handleReadableEof(jsg::Lock& js, jsg::Promise<void> onEof);
  // Sets up relevant callbacks to handle the case when the readable stream reaches EOF.

  // Resolves the `closed` promise when `disconnected` (from watchForDisconnect()) reports a
  // disconnect. This attaches jsg `.then()` continuations, so it must run in a JS-executing context:
  // directly in setupSocket(), or deferred to a microtask in deserialize() (which runs under a scope
  // that forbids JS execution).
  void wireClosedToDisconnect(jsg::Lock& js, kj::Promise<bool> disconnected);

  // Observes the `opened` promise and records its settled state in `openedState`. Must be called
  // after allocation (it uses JSG_THIS, which requires an initialized refcount). This lets
  // serialize() reject transfers of sockets that haven't finished connecting.
  void trackOpenedState(jsg::Lock& js);

  // RPC serialization support
  void serialize(jsg::Lock& js, jsg::Serializer& serializer);

  // Claims the socket prebuilt by RpcDeserializerExternalHandler::prepare() (see
  // hydrateRpcSocket below), or, when the rpc-externals-hydration autogate is off, constructs
  // it in place. The TypeHandler unwraps the prebuilt slot's wrapped socket -- an
  // internal-field read, safe under the deserializer's no-JS scope.
  static jsg::Ref<Socket> deserialize(jsg::Lock& js,
      rpc::SerializationTag tag,
      jsg::Deserializer& deserializer,
      const jsg::TypeHandler<jsg::Ref<Socket>>& socketHandler);

  JSG_RESOURCE_TYPE(Socket) {
    JSG_READONLY_PROTOTYPE_PROPERTY(readable, getReadable);
    JSG_READONLY_PROTOTYPE_PROPERTY(writable, getWritable);
    JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
    JSG_READONLY_PROTOTYPE_PROPERTY(opened, getOpened);
    JSG_READONLY_PROTOTYPE_PROPERTY(upgraded, getUpgraded);
    JSG_READONLY_PROTOTYPE_PROPERTY(secureTransport, getSecureTransport);
    // non-standard extension, not part of the proposed sockets spec
    JSG_READONLY_PROTOTYPE_PROPERTY(protocol, getProtocol);
    JSG_METHOD(close);
    JSG_METHOD(startTls);

    JSG_TS_OVERRIDE({
      get secureTransport(): 'on' | 'off' | 'starttls';
      get protocol(): 'tcp' | 'udp';
    });
  }

  JSG_SERIALIZABLE(rpc::SerializationTag::SOCKET);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("connectionData", sizeof(IoOwn<ConnectionData>));
    readable.visitForMemoryInfo(tracker);
    writable.visitForMemoryInfo(tracker);
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
  struct ConnectionData {
    // A TCP socket's underlying stream, or a UDP socket's underlying datagram channel. UDP
    // sockets have no AsyncIoStream: DatagramChannel::receive()/send() replace tryRead()/write()
    // so that datagram boundaries can never be merged or split.
    kj::OneOf<kj::Rc<kj::AsyncIoStream>, kj::Rc<DatagramChannel>> connectionStream;
    kj::Maybe<kj::Promise<void>> watchForDisconnectTask;
    // tlsStarter must be declared after connectionStream so that it is destroyed first, since it
    // holds a reference that keeps the connection alive.
    kj::Own<kj::TlsStarterCallback> tlsStarter;
    ConnectionData(kj::Own<kj::TlsStarterCallback> tlsStarter,
        kj::OneOf<kj::Rc<kj::AsyncIoStream>, kj::Rc<DatagramChannel>> connectionStream,
        kj::Promise<void> disconnectTask)
        : connectionStream(kj::mv(connectionStream)),
          watchForDisconnectTask(kj::mv(disconnectTask)),
          tlsStarter(kj::mv(tlsStarter)) {}
  };
  kj::Maybe<IoOwn<ConnectionData>> connectionData;

  JsReadableStream readable;
  JsWritableStream writable;
  // This fulfiller is used to resolve the `closedPromise` below.
  jsg::Promise<void>::Resolver closedResolver;
  // Copy kept so that it can be returned from `close`.
  jsg::Promise<void> closedPromiseCopy;
  // Memoized copy that is returned by the `closed` attribute.
  jsg::MemoizedIdentity<jsg::Promise<void>> closedPromise;
  jsg::Optional<SocketOptions> options;
  kj::Maybe<kj::String> remoteAddress;
  kj::Maybe<kj::String> localAddress;
  // Set to true when the socket is upgraded to a secure one.
  bool upgraded = false;
  SecureTransportKind secureTransport;
  SocketProtocol protocol;
  // The domain/ip this socket is connected to. Used for startTls.
  kj::Maybe<kj::String> domain;
  // Whether the port this socket connected to is 80/443. Used for nicer errors.
  bool isDefaultFetchPort;
  // This fulfiller is used to resolve the `openedPromise` below.
  jsg::Promise<SocketInfo>::Resolver openedResolver;
  // Copy kept so that it can be used in `close`.
  jsg::Promise<void> openedPromiseCopy;
  jsg::MemoizedIdentity<jsg::Promise<SocketInfo>> openedPromise;
  // Used to keep track of a pending `close` operation on the socket.
  bool isClosing = false;

  // Tracks the settled state of `openedPromise`. A Socket can only be serialized for RPC transfer
  // once its connection has been established (OPENED); serializing while still PENDING or after a
  // FAILED connection throws, since serialize() is synchronous and cannot await `opened`.
  enum class OpenedState : uint8_t { PENDING, OPENED, FAILED };
  OpenedState openedState = OpenedState::PENDING;

  // Materializes transferred sockets (including marking them OPENED); see its declaration below.
  friend jsg::Ref<Socket> hydrateRpcSocket(jsg::Lock& js,
      IoContext& ioContext,
      rpc::JsValue::External::Reader socketExternal,
      rpc::JsValue::External::Reader readableExternal,
      rpc::JsValue::External::Reader writableExternal);

  kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection();
  jsg::Promise<void> maybeCloseWriteSide(jsg::Lock& js);
  jsg::Promise<void> closeImplOld(jsg::Lock& js);
  jsg::Promise<void> closeImplNew(jsg::Lock& js);

  // Helper method for handleProxyStatus implementations.
  void handleProxyError(jsg::Lock& js, kj::Exception e);

  void resolveFulfiller(jsg::Lock& js, kj::Maybe<kj::Exception> maybeErr) {
    KJ_IF_SOME(err, maybeErr) {
      closedResolver.reject(js, err.clone());
    } else {
      closedResolver.resolve(js);
    }
  };

  void errorHandler(jsg::Lock& js, jsg::Value err) {
    auto jsException = err.getHandle(js);
    resolveFulfiller(js, jsg::createTunneledException(js.v8Isolate, jsException));
  };

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(readable, writable, closedResolver, closedPromiseCopy, closedPromise,
        openedResolver, openedPromiseCopy, openedPromise);
  }
};

jsg::Ref<Socket> setupSocket(jsg::Lock& js,
    kj::Own<kj::AsyncIoStream> connection,
    kj::Maybe<kj::String> remoteAddress,
    kj::Maybe<kj::String> localAddress,
    jsg::Optional<SocketOptions> options,
    kj::Own<kj::TlsStarterCallback> tlsStarter,
    SecureTransportKind secureTransport,
    kj::Maybe<kj::String> domain,
    bool isDefaultFetchPort,
    kj::Maybe<jsg::PromiseResolverPair<SocketInfo>> maybeOpenedPrPair);

jsg::Ref<Socket> setupSocket(jsg::Lock& js,
    kj::Own<kj::AsyncIoStream> connection,
    kj::Maybe<kj::String> remoteAddress,
    kj::Maybe<kj::String> localAddress,
    jsg::Optional<SocketOptions> options,
    kj::Own<kj::TlsStarterCallback> tlsStarter,
    SecureTransportKind secureTransport,
    SocketProtocol protocol,
    kj::Maybe<kj::String> domain,
    bool isDefaultFetchPort,
    kj::Maybe<jsg::PromiseResolverPair<SocketInfo>> maybeOpenedPrPair);

// Builds a datagram (UDP) Socket around `channel`. Unlike setupSocket(), the readable and
// writable streams are value-mode: each chunk read or written corresponds to exactly one
// datagram, since kj::AsyncIoStream's byte-stream semantics cannot preserve datagram boundaries.
// There is no secureTransport/startTls surface (always SecureTransportKind::OFF) and no
// allowHalfOpen option: `closed` resolves once the readable side reaches EOF (the channel's flow
// has ended) and the writable side has been closed in response.
jsg::Ref<Socket> setupDatagramSocket(jsg::Lock& js,
    kj::Own<DatagramChannel> channel,
    kj::Maybe<kj::String> remoteAddress,
    kj::Maybe<kj::String> localAddress);

// A WorkerInterface::CustomEvent that delivers a single UDP flow to a worker's exported
// `connect(socket)` handler. Unlike TCP ingress, this bypasses kj::HttpService::connect()
// entirely (there is no CONNECT tunnel, no headers, and no ConnectResponse to accept/reject): the
// listener that owns `channel` constructs this event directly and calls
// WorkerInterface::customEvent() with it, exactly as Queue/Alarm/Scheduled events do for their own
// non-HTTP-shaped triggers.
//
// This event cannot be forwarded over RPC: a DatagramChannel is a live, in-process-only object,
// so sendRpc() is unimplemented. It is only ever dispatched by a listener running in the same
// process as the worker.
//
// `channel` is borrowed, not owned: the listener that constructs this event attaches the
// underlying flow's ownership to the same task that dispatches this event (see
// Server::UdpListener::dispatch()), so it is guaranteed to outlive every call made through this
// event, exactly as kj::HttpService::connect()'s `connection` reference outlives its dispatch.
class UdpConnectCustomEvent final: public WorkerInterface::CustomEvent {
 public:
  UdpConnectCustomEvent(kj::String host, DatagramChannel& channel)
      : host(kj::mv(host)),
        channel(channel) {}

  kj::Promise<Result> run(kj::Own<IoContext_IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      kj::Maybe<Worker_VersionInfo> versionInfo,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks,
      bool isDynamicDispatch) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      FrankenvalueHandler& frankenvalueHandler,
      rpc::EventDispatcher::Client dispatcher) override {
    KJ_UNIMPLEMENTED(
        "a UDP connect event cannot be forwarded over RPC; it is only ever dispatched in-process "
        "by the listener that owns the underlying datagram flow");
  }

  kj::Promise<Result> notSupported() override {
    KJ_UNIMPLEMENTED("udp connect event not supported");
  }

  static constexpr uint16_t EVENT_TYPE = 14;
  uint16_t getType() override {
    return EVENT_TYPE;
  }

  tracing::EventInfo getEventInfo() const override;

 private:
  kj::String host;
  DatagramChannel& channel;
};

jsg::Ref<Socket> connectImpl(jsg::Lock& js,
    kj::Maybe<jsg::Ref<Fetcher>> fetcher,
    AnySocketAddress address,
    jsg::Optional<SocketOptions> options);

// Materializes a socket received over RPC from its three external-table entries (socket
// metadata, then the readable and writable stream halves, in Socket::serialize()'s order),
// validating the entry types. Runs during RpcDeserializerExternalHandler::prepare() -- before
// the V8 graph read -- because stream construction executes JavaScript under the TypeScript
// streams implementation, which the graph read forbids; Socket::deserialize() then claims the
// result. Also serves as Socket::deserialize()'s in-place fallback when the
// rpc-externals-hydration autogate is off (that path predates the gate and remains scope-safe
// only for legacy streams).
jsg::Ref<Socket> hydrateRpcSocket(jsg::Lock& js,
    IoContext& ioContext,
    rpc::JsValue::External::Reader socketExternal,
    rpc::JsValue::External::Reader readableExternal,
    rpc::JsValue::External::Reader writableExternal);

class SocketsModule final: public jsg::Object {
 public:
  SocketsModule() = default;
  SocketsModule(jsg::Lock&, const jsg::Url&) {}

  jsg::Ref<Socket> connect(
      jsg::Lock& js, AnySocketAddress address, jsg::Optional<SocketOptions> options);

  // Creates a Fetcher from a Socket that can perform HTTP requests over the socket connection
  jsg::Promise<jsg::Ref<Fetcher>> internalNewHttpClient(jsg::Lock& js, jsg::Ref<Socket> socket);

  // Returns the synthetic IP registered for a magic hostname, or undefined. Used by node:dns.
  jsg::Optional<kj::StringPtr> getCallerDnsOverride(jsg::Lock& js, kj::String hostname);

  JSG_RESOURCE_TYPE(SocketsModule, CompatibilityFlags::Reader flags) {
    JSG_METHOD(connect);
    JSG_METHOD(getCallerDnsOverride);

    if (flags.getWorkerdExperimental()) {
      JSG_METHOD(internalNewHttpClient);
    }
  }
};

template <class Registry>
void registerSocketsModule(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<SocketsModule>(
      "cloudflare-internal:sockets", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalSocketModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "cloudflare-internal:sockets"_url;
  builder.addObject<SocketsModule, TypeWrapper>(kSpecifier);
  return builder.finish();
}

#define EW_SOCKETS_ISOLATE_TYPES                                                                   \
  api::Socket, api::SocketOptions, api::SocketAddress, api::TlsOptions, api::SocketsModule,        \
      api::SocketInfo, api::Datagram

// The list of sockets.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
