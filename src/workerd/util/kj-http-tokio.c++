// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// kj's HTTP implementation entry points under --//:io_backend=rust, over kj-hyper (hyper, rustls,
// tokio-tungstenite). kj-http-impl is not linked; only kj-http-types (headers, interfaces, the
// text codec) is. Every existing kj::newHttpClient()/kj::HttpServer call site links unchanged.

#if WORKERD_RUST_IO_BACKEND_RUST

#include <kj-hyper/hyper-http.h>

#include <kj/compat/http.h>
#include <kj/debug.h>

namespace kj {
namespace {

using workerd::rust::kj_hyper::HttpConnector;

// Dials a single kj address (whatever the request URL says).
class AddressConnector final: public HttpConnector {
 public:
  explicit AddressConnector(kj::NetworkAddress& address): address(address) {}
  kj::Promise<kj::Own<kj::AsyncIoStream>> connect(kj::StringPtr authority, bool https) override {
    return address.connect();
  }

 private:
  kj::NetworkAddress& address;
};

// Dials each request's authority over the network (tlsNetwork for https), so restricted networks'
// peer filters apply.
class NetworkConnector final: public HttpConnector {
 public:
  NetworkConnector(kj::Network& network, kj::Maybe<kj::Network&> tlsNetwork)
      : network(network),
        tlsNetwork(tlsNetwork) {}
  kj::Promise<kj::Own<kj::AsyncIoStream>> connect(kj::StringPtr authority, bool https) override {
    kj::Network& net =
        https ? KJ_REQUIRE_NONNULL(tlsNetwork, "this HttpClient doesn't support HTTPS") : network;
    auto address = co_await net.parseAddress(authority, https ? 443 : 80);
    co_return co_await address->connect();
  }

 private:
  kj::Network& network;
  kj::Maybe<kj::Network&> tlsNetwork;
};

// kj's address client sends each URL as given. The pooling client keys connections by absolute
// URL, so origin-form requests are made absolute and sent in origin form; absolute ones go through
// a proxy-form pool (created on first use) that keeps them absolute.
class AddressClient final: public HttpClient {
 public:
  AddressClient(
      const HttpHeaderTable& table, kj::NetworkAddress& address, HttpClientSettings settings)
      : table(table),
        address(address),
        settings(kj::mv(settings)),
        origin(workerd::rust::kj_hyper::newHttpClient(
            table, kj::heap<AddressConnector>(address), this->settings)) {}

  Request request(HttpMethod method,
      kj::StringPtr url,
      const HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    if (url.startsWith("/")) {
      return origin->request(method, absolute(url, headers), headers, expectedBodySize);
    }
    return proxy().request(method, url, headers, expectedBodySize);
  }
  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const HttpHeaders& headers) override {
    if (url.startsWith("/")) return origin->openWebSocket(absolute(url, headers), headers);
    return proxy().openWebSocket(url, headers);
  }
  ConnectRequest connect(
      kj::StringPtr host, const HttpHeaders& headers, HttpConnectSettings settings) override {
    return origin->connect(host, headers, kj::mv(settings));
  }

 private:
  static kj::String absolute(kj::StringPtr url, const HttpHeaders& headers) {
    return kj::str("http://", headers.get(HttpHeaderId::HOST).orDefault("localhost"), url);
  }
  HttpClient& proxy() {
    KJ_IF_SOME(p, proxyClient) return *p;
    return *proxyClient.emplace(workerd::rust::kj_hyper::newHttpClient(
        table, kj::heap<AddressConnector>(address), settings, true));
  }

  const HttpHeaderTable& table;
  kj::NetworkAddress& address;
  HttpClientSettings settings;
  kj::Own<HttpClient> origin;
  kj::Maybe<kj::Own<HttpClient>> proxyClient;
};

// A connection that starts plaintext and can switch to TLS (the sockets API's startTls()).
class TransitionaryAsyncIoStream final: public kj::AsyncIoStream {
 public:
  explicit TransitionaryAsyncIoStream(kj::Own<kj::AsyncIoStream> stream)
      : inner(kj::heap<kj::PausableReadAsyncIoStream>(kj::mv(stream))) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes);
  }
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return inner->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return inner->write(pieces);
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }
  void shutdownWrite() override {
    inner->shutdownWrite();
  }
  void abortRead() override {
    inner->abortRead();
  }

  void startTls(kj::SecureNetworkWrapper& wrapper, kj::StringPtr hostname) {
    inner->pause();
    KJ_ON_SCOPE_FAILURE(inner->reject(KJ_EXCEPTION(FAILED, "StartTls failed.")));
    KJ_REQUIRE(!inner->getCurrentlyReading() && !inner->getCurrentlyWriting(),
        "Cannot call startTls while reads/writes are outstanding");
    inner->replaceStream(kj::newPromisedStream(wrapper.wrapClient(inner->takeStream(), hostname)));
    inner->unpause();
  }

 private:
  kj::Own<kj::PausableReadAsyncIoStream> inner;
};

// kj::newHttpClient(timer, table, network, tlsNetwork): HTTP through the pooling client; connect()
// dials directly (no HTTP on the wire), with startTls support.
class NetworkClient final: public HttpClient {
 public:
  NetworkClient(const HttpHeaderTable& table,
      kj::Network& network,
      kj::Maybe<kj::Network&> tlsNetwork,
      HttpClientSettings settings)
      : table(table),
        network(network),
        tlsNetwork(tlsNetwork),
        settings(kj::mv(settings)),
        inner(workerd::rust::kj_hyper::newHttpClient(
            table, kj::heap<NetworkConnector>(network, tlsNetwork), this->settings)) {}

  Request request(HttpMethod method,
      kj::StringPtr url,
      const HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    return inner->request(method, url, headers, expectedBodySize);
  }
  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const HttpHeaders& headers) override {
    return inner->openWebSocket(url, headers);
  }

  ConnectRequest connect(kj::StringPtr host,
      const HttpHeaders& headers,
      HttpConnectSettings connectSettings) override {
    kj::Network& net = connectSettings.useTls
        ? KJ_REQUIRE_NONNULL(tlsNetwork, "this HttpClient doesn't support TLS")
        : network;
    auto connection =
        kj::newPromisedStream(net.parseAddress(host).then([](kj::Own<kj::NetworkAddress> address) {
      return address->connect().attach(kj::mv(address));
    }));
    KJ_IF_SOME(wrapper, settings.tlsContext) {
      KJ_IF_SOME(starter, connectSettings.tlsStarter) {
        auto transitionary = kj::rc<TransitionaryAsyncIoStream>(kj::mv(connection));
        starter = [&wrapper, ref = transitionary.addRef()](kj::StringPtr hostname) mutable {
          ref->startTls(wrapper, hostname);
          return kj::READY_NOW;
        };
        connection = transitionary.toOwn();
      }
    }
    return {
      ConnectRequest::Status(200, kj::str("OK"), kj::heap<HttpHeaders>(table)), kj::mv(connection)};
  }

 private:
  const HttpHeaderTable& table;
  kj::Network& network;
  kj::Maybe<kj::Network&> tlsNetwork;
  HttpClientSettings settings;
  kj::Own<HttpClient> inner;
};

template <typename T>
kj::Own<T> fakeOwn(T& ref) {
  return kj::Own<T>(&ref, kj::NullDisposer::instance);
}

}  // namespace

kj::Own<HttpClient> newHttpClient(kj::Timer& timer,
    const HttpHeaderTable& responseHeaderTable,
    kj::Network& network,
    kj::Maybe<kj::Network&> tlsNetwork,
    HttpClientSettings settings) {
  return kj::heap<NetworkClient>(responseHeaderTable, network, tlsNetwork, kj::mv(settings));
}

kj::Own<HttpClient> newHttpClient(kj::Timer& timer,
    const HttpHeaderTable& responseHeaderTable,
    kj::NetworkAddress& addr,
    HttpClientSettings settings) {
  return kj::heap<AddressClient>(responseHeaderTable, addr, kj::mv(settings));
}

kj::Own<HttpClient> newHttpClient(const HttpHeaderTable& responseHeaderTable,
    kj::AsyncIoStream& stream,
    HttpClientSettings settings) {
  return workerd::rust::kj_hyper::newHttpClient(responseHeaderTable, stream, settings);
}

// =======================================================================================
// kj::HttpServer

HttpServer::HttpServer(kj::Timer& timer,
    const HttpHeaderTable& requestHeaderTable,
    HttpService& service,
    Settings settings)
    : HttpServer(
          timer, requestHeaderTable, &service, settings, kj::newPromiseAndFulfiller<void>()) {}

HttpServer::HttpServer(kj::Timer& timer,
    const HttpHeaderTable& requestHeaderTable,
    HttpServiceFactory serviceFactory,
    Settings settings)
    : HttpServer(timer,
          requestHeaderTable,
          kj::mv(serviceFactory),
          settings,
          kj::newPromiseAndFulfiller<void>()) {}

HttpServer::HttpServer(kj::Timer& timer,
    const HttpHeaderTable& requestHeaderTable,
    kj::OneOf<HttpService*, HttpServiceFactory> service,
    Settings settings,
    kj::PromiseFulfillerPair<void> paf)
    : timer(timer),
      requestHeaderTable(requestHeaderTable),
      service(kj::mv(service)),
      settings(settings),
      onDrain(paf.promise.fork()),
      drainFulfiller(kj::mv(paf.fulfiller)),
      tasks(*this) {}

void HttpServer::taskFailed(kj::Exception&& exception) {
  KJ_LOG(ERROR, "unhandled exception in HTTP server", exception);
}

kj::Promise<void> HttpServer::drain() {
  KJ_REQUIRE(!draining, "you can only call drain() once");
  draining = true;
  drainFulfiller->fulfill();
  if (connectionCount == 0) return kj::READY_NOW;
  auto paf = kj::newPromiseAndFulfiller<void>();
  zeroConnectionsFulfiller = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

kj::Promise<bool> HttpServer::listenHttpImpl(kj::AsyncIoStream& connection, bool wantCleanDrain) {
  connectionCount++;
  KJ_DEFER({
    if (--connectionCount == 0) {
      KJ_IF_SOME(f, zeroConnectionsFulfiller) f->fulfill();
    }
  });
  kj::Own<HttpService> ownService;
  HttpService* servicePtr;
  KJ_SWITCH_ONEOF(service) {
    KJ_CASE_ONEOF(ptr, HttpService*) {
      servicePtr = ptr;
    }
    KJ_CASE_ONEOF(factory, HttpServiceFactory) {
      ownService = factory(connection);
      servicePtr = ownService.get();
    }
  }
  // listenHttp() and listenLoop() own the stream and only destroy it once this settles, so hyper
  // may take its socket; listenHttpCleanDrain()'s caller, or a factory that saw the stream, keeps
  // using it, so it is only lent. (kj's http.h fixes this private member's signature.)
  auto conn = !wantCleanDrain && ownService.get() == nullptr
      ? workerd::rust::kj_hyper::newHttpConnection(
            requestHeaderTable, *servicePtr, settings, fakeOwn(connection))
      : workerd::rust::kj_hyper::newHttpConnection(
            requestHeaderTable, *servicePtr, settings, connection);
  auto listener =
      onDrain.addBranch().then([&c = *conn]() { c.shutdown(); }).eagerlyEvaluate(nullptr);
  if (draining) conn->shutdown();
  co_await conn->serve();
  co_return false;
}

kj::Promise<void> HttpServer::listenHttp(kj::Own<kj::AsyncIoStream> connection) {
  auto promise = listenHttpImpl(*connection, false).ignoreResult();
  return promise.attach(kj::mv(connection));
}

kj::Promise<bool> HttpServer::listenHttpCleanDrain(kj::AsyncIoStream& connection) {
  return listenHttpImpl(connection, true);
}

kj::Promise<void> HttpServer::listenLoop(kj::ConnectionReceiver& port) {
  for (;;) {
    auto connection = co_await port.accept();
    auto& ref = *connection;
    tasks.add(listenHttpImpl(ref, false).ignoreResult().attach(kj::mv(connection)));
  }
}

kj::Promise<void> HttpServer::listenHttp(kj::ConnectionReceiver& port) {
  return listenLoop(port);
}

}  // namespace kj

#endif  // WORKERD_RUST_IO_BACKEND_RUST
