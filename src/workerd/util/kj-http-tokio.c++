// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Provides the kj-http implementation *symbols* for the rust I/O backend, so every existing
// kj::newHttpClient()/kj::HttpServer call site links unchanged under --//:io_backend=rust --
// no per-call-site #if, no divergence from upstream kj (kj/compat/http.h is untouched). The
// exact sibling of setup-async-io-tokio.c++, one layer up the stack.
//
// Under --//:io_backend=rust the concrete C++ HTTP implementation
// (@capnp-cpp//src/kj/compat:kj-http-impl, i.e. kj/compat/http.c++) is NOT
// linked; only :kj-http-types (the types: header table/objects, interface
// defaults) is. This TU supplies hyper-backed definitions
// of the implementation entry points workerd uses:
//
//  - kj::newHttpClient(table, stream&): the single-connection client. The stream is handed to
//    one hyper HTTP/1.1 client connection (src/rust/cxx/kj-hyper's stream client, which
//    adapts any kj stream -- native tokio socket or duplex pump), consumed natively in the
//    client shape (HyperHttpClient; no service adapter). Once that connection closes, further
//    requests fail DISCONNECTED, matching kj's client whose stream hit EOF.
//  - kj::newHttpClient(timer, table, addr&): the reconnecting client, with kj's per-address
//    connection pool (NetworkAddressHttpClient, ported below): each pooled connection is a
//    single-connection hyper client over a stream dialed from `addr`; checkout discards
//    connections idle past settings.idleTimeout (kj's default 5s, tracked via the passed
//    timer), reuses a live one, or dials.
//  - kj::newHttpClient(HttpService&) / kj::newHttpService(HttpClient&): the client<->service
//    shape adapters (kj evicted its HttpClientAdapter/HttpServiceAdapter into kj-http-impl).
//    Our own hyper-backed shapes compose inside Rust (a shared handle to the same client);
//    foreign C++ objects go through ports of kj's adapters. See the adapter section below.
//  - kj::newWebSocketPipe(): the in-memory WebSocket pair (api's WebSocketPair,
//    http-over-capnp), backed by the Rust pipe state machine in kj-hyper (ws_pipe.rs).
//  - kj::HttpServer: every connection is served by the hyper inbound server
//    (newHyperHttpConnection, which takes the socket natively or bridges in-memory streams
//    through the duplex pump tier). drain() maps to per-connection graceful shutdown.
//
//  - The HTTP/1.1 text codec: the HttpHeaders serialize*/toString/tryParse/parseHeaders
//    members, tryParseHttpRangeHeader, and the HttpMethod stringify/parse utilities, defined
//    over the byte-exact rust ports in kj-rs-http's codec.rs; plus the kj::WebSocket
//    pumpTo()/tryPumpFrom() interface defaults over kj-hyper's websocket_default_pump. See the
//    codec section below.
//
//  - kj::newHttpClient(timer, table, network, tlsNetwork, settings): the proxy-style network
//    client, a port of kj's NetworkHttpClient shape: a per-"scheme:host" cache of pooled
//    per-address clients (each host's address parsed once over the scheme's network -- the
//    tlsNetwork, i.e. the rustls wrapper under this backend, for https -- so a RESTRICTED
//    network's restrictPeers rules apply to every dial), evicted when drained like kj's.
//    connect() dials directly (no HTTP on the wire) and supports the sockets API's startTls
//    via settings.tlsContext + connectSettings.tlsStarter, exactly as kj's client does
//    (TransitionaryAsyncIoStream, ported below onto the public
//    kj::PausableReadAsyncIoStream).
//
// DELIBERATELY NOT DEFINED: kj::newHttpInputStream() and kj::newWebSocket(). Nothing links
// them under the rust backend. If a new call site appears, the build fails with an undefined
// symbol instead of silently serving traffic on a C++ HTTP implementation; implement the
// shape here (hyper-backed) if it is ever needed.
//
// The whole TU is gated on WORKERD_RUST_IO_BACKEND_RUST so that in the default (cxx) build it
// is empty and cannot ODR-clash with kj-http-impl's real definitions. In the rust config the
// reverse hazard (kj-http-impl accidentally linked back in) is guarded by the build-graph gate
// //src/workerd/server:rust-io-hermeticity, which forbids :kj-http-impl.

#if WORKERD_RUST_IO_BACKEND_RUST

#include <kj-hyper/hyper-http.h>
#include <kj-rs-http/http.rs.h>

#include <kj/compat/http.h>
#include <kj/compat/url.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/refcount.h>

#include <deque>
#include <map>

namespace kj {

namespace {

// A borrowed object as a kj::Own (the callee requires an Own; the caller guarantees the
// referent outlives it, exactly as kj-http's own newHttpClient(table, stream&) does).
template <typename T>
kj::Own<T> fakeOwn(T& ref) {
  return kj::Own<T>(&ref, kj::NullDisposer::instance);
}

// Whether WebSocket protocol errors should be rendered the way workerd's JsgifyWebSocketErrors
// handler does. The hyper client cannot call back into an arbitrary C++
// HttpClientErrorHandler; workerd only ever installs JsgifyWebSocketErrors, so the presence of
// a handler selects the jsgified rendering (and its absence kj's default rendering).
bool jsgifyFor(const HttpClientSettings& settings) {
  return settings.webSocketErrorHandler != kj::none;
}

// The kj::HttpClient behind the reconnecting-client shape (kj::newHttpClient(timer, table,
// addr)): a port of kj's NetworkAddressHttpClient, with each pooled connection being a
// one-connection hyper stream client — client shape end to end, no service adapter. Checkout
// first discards connections idle past the idle timeout (kj's settings.idleTimeout semantics,
// timestamped via the passed timer), then reuses the most recently returned live connection,
// else dials `addr` over the selected (tokio) network. The dial is awaited before the stream
// client is created (rather than wrapping the connect promise as kj does) so a native tokio
// socket is handed to hyper whole and keeps the native-socket fast path; like kj's client,
// request()/connect() still return immediately with promised streams while checkout proceeds.
class HyperAddressHttpClient final: public HttpClient {
 public:
  HyperAddressHttpClient(kj::Timer& timer,
      const HttpHeaderTable& responseHeaderTable,
      kj::Own<kj::NetworkAddress> address,
      kj::Duration idleTimeout,
      bool jsgify)
      : timer(timer),
        responseHeaderTable(responseHeaderTable),
        address(kj::mv(address)),
        idleTimeout(idleTimeout),
        jsgify(jsgify) {}

  // Whether there are no open connections (kj's NetworkAddressHttpClient::isDrained()).
  bool isDrained() {
    return activeConnectionCount == 0 && availableClients.empty();
  }

  // Resolves the next time isDrained() transitions from false to true.
  kj::Promise<void> onDrained() {
    auto paf = kj::newPromiseAndFulfiller<void>();
    drainedFulfiller = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }

  Request request(HttpMethod method,
      kj::StringPtr url,
      const HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    auto split = startRequest(method, kj::str(url), headers.clone(), expectedBodySize).split();
    return Request{
      kj::newPromisedStream(kj::mv(kj::get<0>(split))),
      kj::mv(kj::get<1>(split)),
    };
  }

  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const HttpHeaders& headers) override {
    auto urlCopy = kj::str(url);
    auto headersCopy = headers.clone();
    auto refcounted = co_await getClient();
    auto response = co_await refcounted->client->openWebSocket(urlCopy, headersCopy);
    // The connection rides along on whichever half carries the session; a completed WebSocket
    // consumed its connection, so the destructor will find it non-reusable and drop it.
    KJ_SWITCH_ONEOF(response.webSocketOrBody) {
      KJ_CASE_ONEOF(ws, kj::Own<WebSocket>) {
        ws = ws.attach(kj::mv(refcounted));
      }
      KJ_CASE_ONEOF(body, kj::Own<kj::AsyncInputStream>) {
        body = body.attach(kj::mv(refcounted));
      }
    }
    co_return kj::mv(response);
  }

  ConnectRequest connect(
      kj::StringPtr host, const HttpHeaders& headers, HttpConnectSettings settings) override {
    auto split = startConnect(kj::str(host), headers.clone(), kj::mv(settings)).split();
    return ConnectRequest{
      kj::mv(kj::get<0>(split)),
      kj::newPromisedStream(kj::mv(kj::get<1>(split))),
    };
  }

 private:
  kj::Timer& timer;
  const HttpHeaderTable& responseHeaderTable;
  kj::Own<kj::NetworkAddress> address;
  kj::Duration idleTimeout;
  bool jsgify;

  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> drainedFulfiller;
  uint activeConnectionCount = 0;

  bool timeoutsScheduled = false;
  kj::Promise<void> timeoutTask = nullptr;

  struct AvailableClient {
    kj::Own<workerd::rust::kj_hyper::HyperHttpClient> client;
    kj::TimePoint expires;
  };

  std::deque<AvailableClient> availableClients;

  // Counts a connection as active while checked out and returns it to the pool (when still
  // reusable) on destruction — kj's RefcountedClient, verbatim.
  struct RefcountedClient final: public kj::Refcounted {
    RefcountedClient(
        HyperAddressHttpClient& parent, kj::Own<workerd::rust::kj_hyper::HyperHttpClient> client)
        : parent(parent),
          client(kj::mv(client)) {
      ++parent.activeConnectionCount;
    }
    ~RefcountedClient() noexcept(false) {
      --parent.activeConnectionCount;
      KJ_IF_SOME(exception,
          kj::runCatchingExceptions([&]() { parent.returnClientToAvailable(kj::mv(client)); })) {
        KJ_LOG(ERROR, exception);
      }
    }

    HyperAddressHttpClient& parent;
    kj::Own<workerd::rust::kj_hyper::HyperHttpClient> client;
  };

  kj::Promise<kj::Own<RefcountedClient>> getClient() {
    // Discard connections idle past the idle timeout (they expire oldest-first).
    auto now = timer.now();
    while (!availableClients.empty() && availableClients.front().expires <= now) {
      availableClients.pop_front();
    }
    for (;;) {
      if (availableClients.empty()) {
        auto connection = co_await address->connect();
        co_return kj::refcounted<RefcountedClient>(*this,
            workerd::rust::kj_hyper::newHyperStreamHttpClient(
                responseHeaderTable, kj::mv(connection), jsgify));
      } else {
        auto client = kj::mv(availableClients.back().client);
        availableClients.pop_back();
        if (client->canReuse()) {
          co_return kj::refcounted<RefcountedClient>(*this, kj::mv(client));
        }
        // Whoops, this client's connection was closed by the server at some point. Discard.
      }
    }
  }

  void returnClientToAvailable(kj::Own<workerd::rust::kj_hyper::HyperHttpClient> client) {
    // Only return the connection to the pool if it is reusable and if our settings indicate we
    // should reuse connections.
    if (client->canReuse() && idleTimeout > 0 * kj::SECONDS) {
      availableClients.push_back(AvailableClient{kj::mv(client), timer.now() + idleTimeout});
    }

    // Call this either way because it also signals onDrained().
    if (!timeoutsScheduled) {
      timeoutsScheduled = true;
      timeoutTask = applyTimeouts();
    }
  }

  kj::Promise<void> applyTimeouts() {
    if (availableClients.empty()) {
      timeoutsScheduled = false;
      if (activeConnectionCount == 0) {
        KJ_IF_SOME(f, drainedFulfiller) {
          f->fulfill();
          drainedFulfiller = kj::none;
        }
      }
      return kj::READY_NOW;
    } else {
      auto time = availableClients.front().expires;
      return timer.atTime(time).then([this, time]() {
        while (!availableClients.empty() && availableClients.front().expires <= time) {
          availableClients.pop_front();
        }
        return applyTimeouts();
      });
    }
  }

  kj::Promise<kj::Tuple<kj::Own<kj::AsyncOutputStream>, kj::Promise<Response>>> startRequest(
      HttpMethod method,
      kj::String url,
      HttpHeaders headers,
      kj::Maybe<uint64_t> expectedBodySize) {
    auto refcounted = co_await getClient();
    auto req = refcounted->client->request(method, url, headers, expectedBodySize);
    req.body = req.body.attach(kj::addRef(*refcounted));
    auto response =
        req.response.then([refcounted = kj::mv(refcounted)](Response&& response) mutable {
      response.body = response.body.attach(kj::mv(refcounted));
      return kj::mv(response);
    });
    co_return kj::tuple(kj::mv(req.body), kj::mv(response));
  }

  kj::Promise<kj::Tuple<ConnectRequest::Status, kj::Own<kj::AsyncIoStream>>> startConnect(
      kj::String host, HttpHeaders headers, HttpConnectSettings settings) {
    auto refcounted = co_await getClient();
    auto req = refcounted->client->connect(host, headers, kj::mv(settings));
    auto status = co_await req.status;
    // An established tunnel consumed its connection (an upgrade), so pooling never applies;
    // the refcount just keeps the connection alive and counted while the tunnel runs.
    co_return kj::tuple(kj::mv(status), req.connection.attach(kj::mv(refcounted)));
  }
};

// kj's TransitionaryAsyncIoStream (private to kj/compat/http.c++), rebuilt on the public
// kj::PausableReadAsyncIoStream: a connection that starts out plaintext and can be switched to
// TLS mid-stream (the sockets API's startTls()). startTls() pauses reads, hands the raw stream
// to the SecureNetworkWrapper (the rustls wrapper under this backend), and resumes over the
// promised secure stream.
class TransitionaryAsyncIoStream final: public kj::AsyncIoStream {
 public:
  explicit TransitionaryAsyncIoStream(kj::Own<kj::AsyncIoStream> unencryptedStream)
      : inner(kj::heap<kj::PausableReadAsyncIoStream>(kj::mv(unencryptedStream))) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes);
  }
  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength();
  }
  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return inner->pumpTo(output, amount);
  }
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return inner->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return inner->write(pieces);
  }
  kj::Maybe<kj::Promise<uint64_t>> tryPumpFrom(
      kj::AsyncInputStream& input, uint64_t amount = kj::maxValue) override {
    return inner->tryPumpFrom(input, amount);
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
  kj::Maybe<int> getFd() const override {
    return inner->getFd();
  }

  void startTls(kj::SecureNetworkWrapper* wrapper, kj::StringPtr expectedServerHostname) {
    // Pause any potential pending reads.
    inner->pause();

    KJ_ON_SCOPE_FAILURE({ inner->reject(KJ_EXCEPTION(FAILED, "StartTls failed.")); });

    KJ_ASSERT(!inner->getCurrentlyReading() && !inner->getCurrentlyWriting(),
        "Cannot call startTls while reads/writes are outstanding");
    kj::Promise<kj::Own<kj::AsyncIoStream>> secureStream =
        wrapper->wrapClient(inner->takeStream(), expectedServerHostname);
    inner->replaceStream(kj::newPromisedStream(kj::mv(secureStream)));
    // Resume any previous pending reads.
    inner->unpause();
  }

 private:
  kj::Own<kj::PausableReadAsyncIoStream> inner;
};

// kj's PromiseNetworkAddressHttpClient: waits for the address (DNS) promise to resolve, then
// forwards all calls to the pooled per-address client built from it.
class PromiseHyperAddressHttpClient final: public HttpClient {
 public:
  PromiseHyperAddressHttpClient(kj::Promise<kj::Own<HyperAddressHttpClient>> promise)
      : promise(promise
                    .then([this](kj::Own<HyperAddressHttpClient>&& client) {
                      this->client = kj::mv(client);
                    })
                    .fork()) {}

  bool isDrained() {
    KJ_IF_SOME(c, client) {
      return c->isDrained();
    } else {
      return failed;
    }
  }

  kj::Promise<void> onDrained() {
    KJ_IF_SOME(c, client) {
      return c->onDrained();
    } else {
      return promise.addBranch().then(
          [this]() { return KJ_ASSERT_NONNULL(client)->onDrained(); }, [this](kj::Exception&& e) {
        // Connecting failed. Treat as immediately drained.
        failed = true;
        return kj::READY_NOW;
      });
    }
  }

  Request request(HttpMethod method,
      kj::StringPtr url,
      const HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    KJ_IF_SOME(c, client) {
      return c->request(method, url, headers, expectedBodySize);
    } else {
      // This gets complicated since request() returns a pair of a stream and a promise.
      auto urlCopy = kj::str(url);
      auto headersCopy = headers.clone();
      auto combined = promise.addBranch().then(
          [this, method, expectedBodySize, url = kj::mv(urlCopy), headers = kj::mv(headersCopy)]()
              -> kj::Tuple<kj::Own<kj::AsyncOutputStream>, kj::Promise<Response>> {
        auto req = KJ_ASSERT_NONNULL(client)->request(method, url, headers, expectedBodySize);
        return kj::tuple(kj::mv(req.body), kj::mv(req.response));
      });

      auto split = combined.split();
      return {kj::newPromisedStream(kj::mv(kj::get<0>(split))), kj::mv(kj::get<1>(split))};
    }
  }

  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const HttpHeaders& headers) override {
    KJ_IF_SOME(c, client) {
      return c->openWebSocket(url, headers);
    } else {
      auto urlCopy = kj::str(url);
      auto headersCopy = headers.clone();
      return promise.addBranch().then(
          [this, url = kj::mv(urlCopy), headers = kj::mv(headersCopy)]() {
        return KJ_ASSERT_NONNULL(client)->openWebSocket(url, headers);
      });
    }
  }

  ConnectRequest connect(
      kj::StringPtr host, const HttpHeaders& headers, HttpConnectSettings settings) override {
    KJ_IF_SOME(c, client) {
      return c->connect(host, headers, kj::mv(settings));
    } else {
      auto split =
          promise.addBranch()
              .then([this, host = kj::str(host), headers = headers.clone(),
                        settings]() mutable -> kj::Tuple<kj::Promise<ConnectRequest::Status>,
                                                kj::Promise<kj::Own<kj::AsyncIoStream>>> {
        auto request = KJ_ASSERT_NONNULL(client)->connect(host, headers, kj::mv(settings));
        return kj::tuple(kj::mv(request.status), kj::mv(request.connection));
      }).split();

      return ConnectRequest{
        kj::mv(kj::get<0>(split)),
        kj::newPromisedStream(kj::mv(kj::get<1>(split))),
      };
    }
  }

 private:
  kj::ForkedPromise<void> promise;
  kj::Maybe<kj::Own<HyperAddressHttpClient>> client;
  bool failed = false;
};

// The kj::HttpClient behind the proxy-style network-client shape (absolute-form request URLs):
// a port of kj's NetworkHttpClient. request()/openWebSocket() parse the URL, convert it to
// origin-form with a Host header, and dispatch to a cached per-host pooled client whose address
// is parsed over the scheme's network (tlsNetwork for https), so a restricted network's
// restrictPeers rules apply to every dial; drained hosts are evicted (and re-resolved on next
// use), as kj's are. connect() is a raw dial — no HTTP bytes on the wire — with startTls
// support via settings.tlsContext, exactly mirroring kj's.
class HyperNetworkHttpClient final: public HttpClient, private kj::TaskSet::ErrorHandler {
 public:
  HyperNetworkHttpClient(kj::Timer& timer,
      const HttpHeaderTable& responseHeaderTable,
      kj::Network& network,
      kj::Maybe<kj::Network&> tlsNetwork,
      HttpClientSettings settings)
      : timer(timer),
        responseHeaderTable(responseHeaderTable),
        network(network),
        tlsNetwork(tlsNetwork),
        settings(kj::mv(settings)),
        tasks(*this) {}

  Request request(HttpMethod method,
      kj::StringPtr url,
      const HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    // We need to parse the proxy-style URL to convert it to host-style.
    // Use URL parsing options that avoid unnecessary rewrites.
    kj::Url::Options urlOptions;
    urlOptions.allowEmpty = true;
    urlOptions.percentDecode = false;

    auto parsed = kj::Url::parse(url, kj::Url::HTTP_PROXY_REQUEST, urlOptions);
    auto path = parsed.toString(kj::Url::HTTP_REQUEST);
    auto headersCopy = headers.clone();
    headersCopy.setPtr(HttpHeaderId::HOST, parsed.host);
    return getClient(parsed).request(method, path, headersCopy, expectedBodySize);
  }

  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const HttpHeaders& headers) override {
    // We need to parse the proxy-style URL to convert it to origin-form.
    // Use URL parsing options that avoid unnecessary rewrites.
    kj::Url::Options urlOptions;
    urlOptions.allowEmpty = true;
    urlOptions.percentDecode = false;

    auto parsed = kj::Url::parse(url, kj::Url::HTTP_PROXY_REQUEST, urlOptions);
    auto path = parsed.toString(kj::Url::HTTP_REQUEST);
    auto headersCopy = headers.clone();
    headersCopy.setPtr(HttpHeaderId::HOST, parsed.host);
    return getClient(parsed).openWebSocket(path, headersCopy);
  }

  ConnectRequest connect(kj::StringPtr host,
      const HttpHeaders& headers,
      HttpConnectSettings connectSettings) override {
    // We want to connect directly instead of going through a proxy here, as kj does.
    kj::Maybe<kj::Promise<kj::Own<kj::NetworkAddress>>> addr;
    if (connectSettings.useTls) {
      kj::Network& tlsNet = KJ_REQUIRE_NONNULL(tlsNetwork, "this HttpClient doesn't support TLS");
      addr = tlsNet.parseAddress(host);
    } else {
      addr = network.parseAddress(host);
    }

    auto split = KJ_ASSERT_NONNULL(addr)
                     .then([this](auto address) {
      return address->connect()
          .then([this](auto connection) -> kj::Tuple<kj::Promise<ConnectRequest::Status>,
                                            kj::Promise<kj::Own<kj::AsyncIoStream>>> {
        return kj::tuple(ConnectRequest::Status(200, kj::str("OK"),
                             kj::heap<kj::HttpHeaders>(responseHeaderTable)  // Empty headers
                             ),
            kj::mv(connection));
      }).attach(kj::mv(address));
    }).split();

    auto connection = kj::newPromisedStream(kj::mv(kj::get<1>(split)));

    if (!connectSettings.useTls) {
      KJ_IF_SOME(wrapper, settings.tlsContext) {
        KJ_IF_SOME(tlsStarter, connectSettings.tlsStarter) {
          kj::Rc<TransitionaryAsyncIoStream> transitConnectionRef(
              kj::heap<TransitionaryAsyncIoStream>(kj::mv(connection)));
          kj::Function<kj::Promise<void>(kj::StringPtr)> cb =
              [&wrapper, ref1 = transitConnectionRef.addRef()](
                  kj::StringPtr expectedServerHostname) mutable {
            ref1->startTls(&wrapper, expectedServerHostname);
            return kj::READY_NOW;
          };
          connection = transitConnectionRef.addRef().toOwn();
          tlsStarter = kj::mv(cb);
        }
      }
    }

    return ConnectRequest{kj::mv(kj::get<0>(split)), kj::mv(connection)};
  }

 private:
  kj::Timer& timer;
  const HttpHeaderTable& responseHeaderTable;
  kj::Network& network;
  kj::Maybe<kj::Network&> tlsNetwork;
  HttpClientSettings settings;

  struct Host {
    kj::String name;  // including port, if non-default
    kj::Own<PromiseHyperAddressHttpClient> client;
  };

  std::map<kj::StringPtr, Host> httpHosts;
  std::map<kj::StringPtr, Host> httpsHosts;

  kj::TaskSet tasks;

  HttpClient& getClient(kj::Url& parsed) {
    bool isHttps = parsed.scheme == "https";
    bool isHttp = parsed.scheme == "http";
    KJ_REQUIRE(isHttp || isHttps);

    auto& hosts = isHttps ? httpsHosts : httpHosts;

    // Look for a cached client for this host.
    auto iter = hosts.find(parsed.host);

    if (iter == hosts.end()) {
      // Need to open a new connection.
      kj::Network* networkToUse = &network;
      if (isHttps) {
        networkToUse = &KJ_REQUIRE_NONNULL(tlsNetwork, "this HttpClient doesn't support HTTPS");
      }

      auto promise = networkToUse->parseAddress(parsed.host, isHttps ? 443 : 80)
                         .then([this](kj::Own<kj::NetworkAddress> addr) {
        return kj::heap<HyperAddressHttpClient>(
            timer, responseHeaderTable, kj::mv(addr), settings.idleTimeout, jsgifyFor(settings));
      });

      Host host{kj::mv(parsed.host), kj::heap<PromiseHyperAddressHttpClient>(kj::mv(promise))};
      kj::StringPtr nameRef = host.name;

      auto insertResult = hosts.insert(std::make_pair(nameRef, kj::mv(host)));
      KJ_ASSERT(insertResult.second);
      iter = insertResult.first;

      tasks.add(handleCleanup(hosts, iter));
    }

    return *iter->second.client;
  }

  kj::Promise<void> handleCleanup(
      std::map<kj::StringPtr, Host>& hosts, std::map<kj::StringPtr, Host>::iterator iter) {
    return iter->second.client->onDrained().then([this, &hosts, iter]() -> kj::Promise<void> {
      // Double-check that it's really drained to avoid race conditions.
      if (iter->second.client->isDrained()) {
        hosts.erase(iter);
        return kj::READY_NOW;
      } else {
        return handleCleanup(hosts, iter);
      }
    });
  }

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, exception);
  }
};

// Mirrors kj::HttpServer::Connection's application-error path: an exception from the service
// is routed to the configured HttpServerErrorHandler (or the interface's default), with the
// response reference passed along only if the service had not yet committed a response head.
// Without this, hyper would send its own generic 500 and the configured handler (workerd's
// Connection / InspectorService implement one) would never run.
class ErrorHandlingService final: public HttpService {
 public:
  ErrorHandlingService(HttpService& inner, const HttpServerSettings& settings)
      : inner(inner),
        settings(settings) {}

  kj::Promise<void> request(HttpMethod method,
      kj::StringPtr url,
      const HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    TrackingResponse tracked(response);
    ClosingResponse closing(response);
    KJ_TRY {
      co_return co_await inner.request(method, url, headers, requestBody, tracked);
    }
    KJ_CATCH(exception) {
      class DefaultHandler final: public HttpServerErrorHandler {};
      DefaultHandler defaultHandler;
      HttpServerErrorHandler& handler =
          const_cast<HttpServerSettings&>(settings).errorHandler.orDefault(defaultHandler);
      kj::Maybe<Response&> maybeResponse;
      if (!tracked.started) maybeResponse = closing;
      co_await handler.handleApplicationError(kj::mv(exception), maybeResponse);
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings connectSettings) override {
    return inner.connect(host, headers, connection, response, kj::mv(connectSettings));
  }

 private:
  // Forwards while recording whether a response head was committed.
  struct TrackingResponse final: public Response {
    explicit TrackingResponse(Response& inner): inner(inner) {}
    Response& inner;
    bool started = false;

    kj::Own<kj::AsyncOutputStream> send(uint statusCode,
        kj::StringPtr statusText,
        const HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize) override {
      started = true;
      return inner.send(statusCode, statusText, headers, expectedBodySize);
    }
    kj::Own<WebSocket> acceptWebSocket(const HttpHeaders& headers) override {
      started = true;
      return inner.acceptWebSocket(headers);
    }
  };

  // Wraps the response handed to the error handler: kj sets closeAfterSend before invoking it,
  // so the error response carries `Connection: close` and the connection drops after it (hyper
  // honors the header the same way).
  struct ClosingResponse final: public Response {
    explicit ClosingResponse(Response& inner): inner(inner) {}
    Response& inner;

    kj::Own<kj::AsyncOutputStream> send(uint statusCode,
        kj::StringPtr statusText,
        const HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize) override {
      auto withClose = headers.cloneShallow();
      withClose.set(HttpHeaderId::CONNECTION, "close");
      return inner.send(statusCode, statusText, withClose, expectedBodySize);
    }
    kj::Own<WebSocket> acceptWebSocket(const HttpHeaders& headers) override {
      return inner.acceptWebSocket(headers);
    }
  };

  HttpService& inner;
  const HttpServerSettings& settings;
};

}  // namespace

WebSocketPipe newWebSocketPipe() {
  return workerd::rust::kj_hyper::newRustWebSocketPipe();
}

kj::Own<HttpClient> newHttpClient(const HttpHeaderTable& responseHeaderTable,
    kj::AsyncIoStream& stream,
    HttpClientSettings settings) {
  return workerd::rust::kj_hyper::newHyperStreamHttpClient(
      responseHeaderTable, fakeOwn(stream), jsgifyFor(settings));
}

kj::Own<HttpClient> newHttpClient(kj::Timer& timer,
    const HttpHeaderTable& responseHeaderTable,
    kj::NetworkAddress& addr,
    HttpClientSettings settings) {
  return kj::heap<HyperAddressHttpClient>(
      timer, responseHeaderTable, fakeOwn(addr), settings.idleTimeout, jsgifyFor(settings));
}

kj::Own<HttpClient> newHttpClient(kj::Timer& timer,
    const HttpHeaderTable& responseHeaderTable,
    kj::Network& network,
    kj::Maybe<kj::Network&> tlsNetwork,
    HttpClientSettings settings) {
  return kj::heap<HyperNetworkHttpClient>(
      timer, responseHeaderTable, network, tlsNetwork, kj::mv(settings));
}

// =======================================================================================
// The client<->service shape adapters: kj::newHttpClient(HttpService&) and
// kj::newHttpService(HttpClient&).
//
// kj evicted its HttpClientAdapter/HttpServiceAdapter into kj-http-impl (kj/compat/http.c++),
// so under the rust backend this TU defines the two entry points. Each has two paths:
//
//  - Fast path: the passed object is one of our hyper-backed shapes (HyperHttpService /
//    HyperHttpClient). Both wrap the same Rust client type, so the adaptation is a new handle
//    to the same Rust client (shared upstream + keep-alive pool) in the other shape — the
//    composition stays inside Rust, with no C++ round-trips per request.
//  - Foreign path: a genuinely foreign C++ object (e.g. workerd's WorkerInterface services,
//    server.c++'s cache client). These are ports of kj's own adapters, byte-for-byte in
//    behavior (workerd's tests assert their exact semantics and exception texts), over public
//    interfaces only: kj's internal AsyncIoStreamWithGuards/HeadResponseStream
//    (private to kj/compat/http.c++, not installed for external consumers) are replicated below minus the
//    released-buffer feature neither adapter uses. The WebSocket pipe both adapters rely on is
//    this TU's kj::newWebSocketPipe() — the Rust pipe — so pumps stay on the rust path.
//
// Lifetime contract (same as kj's): the returned client/service *borrows* the passed
// service/client; the caller keeps it alive.

namespace {

// kj's HeadResponseStream (private to kj/compat/http.c++): an input stream which returns no data, but
// tryGetLength() reports the specified value — HEAD responses, where the size is known but no
// body is sent.
class HeadResponseStream final: public kj::AsyncInputStream {
 public:
  HeadResponseStream(kj::Maybe<size_t> expectedLength): expectedLength(expectedLength) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return kj::constPromise<size_t, 0>();
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return expectedLength;
  }

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return kj::constPromise<uint64_t, 0>();
  }

 private:
  kj::Maybe<size_t> expectedLength;
};

// kj's AsyncIoStreamWithGuards (private to kj/compat/http.c++), minus the released-buffer read-guard payload
// the adapters never use: separate promise guards on the input and output, delaying reads and
// writes until the relevant guard resolves. The CONNECT adapters use it to gate one direction
// of the tunnel on accept/reject: if the guard rejects, the stream is permanently errored and
// pending reads/writes fail with that exception.
class GuardedAsyncIoStream final: public kj::AsyncIoStream, private kj::TaskSet::ErrorHandler {
 public:
  GuardedAsyncIoStream(
      kj::Own<kj::AsyncIoStream> inner, kj::Promise<void> readGuard, kj::Promise<void> writeGuard)
      : inner(kj::mv(inner)),
        readGuard(handleReadGuard(kj::mv(readGuard))),
        writeGuard(handleWriteGuard(kj::mv(writeGuard))),
        tasks(*this) {}

  // AsyncInputStream

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    if (readGuardReleased) {
      return inner->tryRead(buffer, minBytes, maxBytes);
    }
    return readGuard.addBranch().then(
        [this, buffer, minBytes, maxBytes] { return inner->tryRead(buffer, minBytes, maxBytes); });
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return kj::none;
  }

  kj::Promise<uint64_t> pumpTo(
      kj::AsyncOutputStream& output, uint64_t amount = kj::maxValue) override {
    if (readGuardReleased) {
      return inner->pumpTo(output, amount);
    }
    return readGuard.addBranch().then(
        [this, &output, amount] { return inner->pumpTo(output, amount); });
  }

  // AsyncOutputStream

  void shutdownWrite() override {
    if (writeGuardReleased) {
      inner->shutdownWrite();
    } else {
      tasks.add(writeGuard.addBranch().then([this]() { inner->shutdownWrite(); }));
    }
  }

  kj::Maybe<kj::Promise<uint64_t>> tryPumpFrom(
      kj::AsyncInputStream& input, uint64_t amount = kj::maxValue) override {
    if (writeGuardReleased) {
      return input.pumpTo(*inner, amount);
    } else {
      return writeGuard.addBranch().then(
          [this, &input, amount]() { return input.pumpTo(*inner, amount); });
    }
  }

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    if (writeGuardReleased) {
      return inner->write(buffer);
    } else {
      return writeGuard.addBranch().then([this, buffer]() { return inner->write(buffer); });
    }
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    if (writeGuardReleased) {
      return inner->write(pieces);
    } else {
      return writeGuard.addBranch().then([this, pieces]() { return inner->write(pieces); });
    }
  }

  kj::Promise<void> whenWriteDisconnected() override {
    if (writeGuardReleased) {
      return inner->whenWriteDisconnected();
    } else {
      return writeGuard.addBranch().then([this]() { return inner->whenWriteDisconnected(); },
          [](kj::Exception&& e) mutable -> kj::Promise<void> {
        if (e.getType() == kj::Exception::Type::DISCONNECTED) {
          return kj::READY_NOW;
        } else {
          return kj::mv(e);
        }
      });
    }
  }

 private:
  kj::Own<kj::AsyncIoStream> inner;
  kj::ForkedPromise<void> readGuard;
  kj::ForkedPromise<void> writeGuard;
  bool readGuardReleased = false;
  bool writeGuardReleased = false;
  kj::TaskSet tasks;
  // Set of tasks used to call `shutdownWrite` after the write guard is released.

  void taskFailed(kj::Exception&& exception) override {
    // Only used for deferred shutdownWrite(); DISCONNECTED is uninteresting there (kj's
    // AsyncIoStreamWithGuards ignores that class too).
    if (exception.getType() != kj::Exception::Type::DISCONNECTED) {
      KJ_LOG(ERROR, exception);
    }
  }

  kj::ForkedPromise<void> handleWriteGuard(kj::Promise<void> guard) {
    return guard.then([this]() { writeGuardReleased = true; }).fork();
  }

  kj::ForkedPromise<void> handleReadGuard(kj::Promise<void> guard) {
    return guard.then([this]() { readGuardReleased = true; }).fork();
  }
};

// kj's HttpClientAdapter (in the unlinked kj-http-impl, not kj-http-types): the foreign path of
// kj::newHttpClient(HttpService&), for real C++ services (workerd's WorkerInterface & co).
// A byte-for-byte behavioral port; only the internal-header helpers were swapped for the
// replicas above, and the WebSocket pipe is the Rust one.
class ForeignServiceHttpClient final: public HttpClient {
 public:
  ForeignServiceHttpClient(HttpService& service): service(service) {}

  Request request(HttpMethod method,
      kj::StringPtr url,
      const HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    // We have to clone the URL and headers because HttpService implementations are allowed to
    // assume that they remain valid until the service handler completes whereas HttpClient
    // callers are allowed to destroy them immediately after the call.
    auto urlCopy = kj::str(url);
    auto headersCopy = kj::heap(headers.clone());

    auto pipe = newOneWayPipe(expectedBodySize);

    auto paf = kj::newPromiseAndFulfiller<Response>();
    auto responder = kj::refcounted<ResponseImpl>(method, kj::mv(paf.fulfiller));

    auto requestPaf = kj::newPromiseAndFulfiller<kj::Promise<void>>();
    responder->setPromise(kj::mv(requestPaf.promise));

    auto promise = service.request(method, urlCopy, *headersCopy, *pipe.in, *responder)
                       .attach(kj::mv(pipe.in), kj::mv(urlCopy), kj::mv(headersCopy));
    requestPaf.fulfiller->fulfill(kj::mv(promise));

    return {kj::mv(pipe.out), paf.promise.attach(kj::mv(responder))};
  }

  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const HttpHeaders& headers) override {
    // We have to clone the URL and headers because HttpService implementations are allowed to
    // assume that they remain valid until the service handler completes whereas HttpClient
    // callers are allowed to destroy them immediately after the call. Also we need to add
    // `Upgrade: websocket` so that headers.isWebSocket() returns true on the service side.
    auto urlCopy = kj::str(url);
    auto headersCopy = kj::heap(headers.clone());
    headersCopy->setPtr(HttpHeaderId::UPGRADE, "websocket");
    KJ_DASSERT(headersCopy->isWebSocket());

    auto paf = kj::newPromiseAndFulfiller<WebSocketResponse>();
    auto responder = kj::refcounted<WebSocketResponseImpl>(kj::mv(paf.fulfiller));

    auto requestPaf = kj::newPromiseAndFulfiller<kj::Promise<void>>();
    responder->setPromise(kj::mv(requestPaf.promise));

    auto in = kj::heap<kj::NullStream>();
    auto promise = service.request(HttpMethod::GET, urlCopy, *headersCopy, *in, *responder)
                       .attach(kj::mv(in), kj::mv(urlCopy), kj::mv(headersCopy));
    requestPaf.fulfiller->fulfill(kj::mv(promise));

    return paf.promise.attach(kj::mv(responder));
  }

  ConnectRequest connect(
      kj::StringPtr host, const HttpHeaders& headers, HttpConnectSettings settings) override {
    // We have to clone the host and the headers because HttpService implementations are allowed
    // to assume that they remain valid until the service handler completes whereas HttpClient
    // callers are allowed to destroy them immediately after the call.
    auto hostCopy = kj::str(host);
    auto headersCopy = kj::heap(headers.clone());

    // 1. Create a new TwoWayPipe, one end will be returned with the ConnectRequest,
    //    the other will be held by the ConnectResponseImpl.
    auto pipe = kj::newTwoWayPipe();

    // 2. Create a promise/fulfiller pair for the status. The promise will be
    //    returned with the ConnectResponse, the fulfiller will be held by the
    //    ConnectResponseImpl.
    auto paf = kj::newPromiseAndFulfiller<ConnectRequest::Status>();

    // 3. Create the ConnectResponseImpl
    auto response =
        kj::refcounted<ConnectResponseImpl>(kj::mv(paf.fulfiller), kj::mv(pipe.ends[0]));

    // 4. Call service.connect, passing in the tunnel.
    //    The call to tunnel->getConnectStream() returns a guarded stream that will buffer
    //    writes until the status is indicated by calling accept/reject.
    auto connectStream = response->getConnectStream();
    auto promise = service.connect(hostCopy, *headersCopy, *connectStream, *response, settings)
                       .eagerlyEvaluate(
                           [response = kj::mv(response), host = kj::mv(hostCopy),
                               headers = kj::mv(headersCopy),
                               connectStream = kj::mv(connectStream)](kj::Exception&& ex) mutable {
      // A few things need to happen here.
      //   1. We'll log the exception.
      //   2. We'll break the pipe.
      //   3. We'll reject the status promise if it is still pending.
      //
      // We'll do all of this within the ConnectResponseImpl, however, since it
      // maintains the state necessary here.
      response->handleException(kj::mv(ex), kj::mv(connectStream));
    });

    // TODO(bug) (inherited from kj's adapter): the client will likely drop the connection as
    // soon as it reads EOF, but the promise representing the service connect() call may still
    // be running and want to do some cleanup after it has sent EOF. That cleanup will be
    // canceled. For regular HTTP calls, DelayedEofInputStream was created to address this
    // exact issue; with connect() being bidirectional a delay would be needed only once both
    // directions have closed, which needs a promise-returning shutdownWrite() alternative.
    return ConnectRequest{
      kj::mv(paf.promise),
      pipe.ends[1].attach(kj::mv(promise)),
    };
  }

 private:
  HttpService& service;

  class DelayedEofInputStream final: public kj::AsyncInputStream {
    // An AsyncInputStream wrapper that, when it reaches EOF, delays the final read until some
    // promise completes.

   public:
    DelayedEofInputStream(kj::Own<kj::AsyncInputStream> inner, kj::Promise<void> completionTask)
        : inner(kj::mv(inner)),
          completionTask(kj::mv(completionTask)) {}

    kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
      return wrap(minBytes, inner->tryRead(buffer, minBytes, maxBytes));
    }

    kj::Maybe<uint64_t> tryGetLength() override {
      return inner->tryGetLength();
    }

    kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
      return wrap(amount, inner->pumpTo(output, amount));
    }

   private:
    kj::Own<kj::AsyncInputStream> inner;
    kj::Maybe<kj::Promise<void>> completionTask;

    template <typename T>
    kj::Promise<T> wrap(T requested, kj::Promise<T> innerPromise) {
      return innerPromise.then([this, requested](T actual) -> kj::Promise<T> {
        if (actual < requested) {
          // Must have reached EOF.
          KJ_IF_SOME(t, completionTask) {
            // Delay until completion.
            auto result = t.then([actual]() { return actual; });
            completionTask = kj::none;
            return result;
          } else {
            // Must have called tryRead() again after we already signaled EOF. Fine.
            return actual;
          }
        } else {
          return actual;
        }
      }, [this](kj::Exception&& e) -> kj::Promise<T> {
        // The stream threw an exception, but this exception is almost certainly just
        // complaining that the other end of the stream was dropped. In all likelihood, the
        // HttpService request() call itself will throw a much more interesting error -- we'd
        // rather propagate that one, if so.
        KJ_IF_SOME(t, completionTask) {
          auto result = t.then([e = kj::mv(e)]() mutable -> kj::Promise<T> {
            // Looks like the service didn't throw. I guess we should propagate the stream
            // error after all.
            return kj::mv(e);
          });
          completionTask = kj::none;
          return result;
        } else {
          // Must have called tryRead() again after we already signaled EOF or threw. Fine.
          return kj::mv(e);
        }
      });
    }
  };

  class ResponseImpl final: public HttpService::Response, public kj::Refcounted {
   public:
    ResponseImpl(
        kj::HttpMethod method, kj::Own<kj::PromiseFulfiller<HttpClient::Response>> fulfiller)
        : method(method),
          fulfiller(kj::mv(fulfiller)) {}

    void setPromise(kj::Promise<void> promise) {
      task = promise.eagerlyEvaluate([this](kj::Exception&& exception) {
        if (fulfiller->isWaiting()) {
          fulfiller->reject(kj::mv(exception));
        } else {
          // We need to cause the response stream's read() to throw this, so we should propagate
          // it.
          kj::throwRecoverableException(kj::mv(exception));
        }
      });
    }

    kj::Own<kj::AsyncOutputStream> send(uint statusCode,
        kj::StringPtr statusText,
        const HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
      // The caller of HttpClient is allowed to assume that the statusText and headers remain
      // valid until the body stream is dropped, but the HttpService implementation is allowed
      // to send values that are only valid until send() returns, so we have to copy.
      auto statusTextCopy = kj::str(statusText);
      auto headersCopy = kj::heap(headers.clone());

      if (method == kj::HttpMethod::HEAD || expectedBodySize.orDefault(1) == 0) {
        // We're not expecting any body. We need to delay reporting completion to the client
        // until the server side has actually returned from the service method, otherwise we may
        // prematurely cancel it.

        task = task.then([this, statusCode, statusTextCopy = kj::mv(statusTextCopy),
                             headersCopy = kj::mv(headersCopy), expectedBodySize]() mutable {
          fulfiller->fulfill({statusCode, statusTextCopy, headersCopy.get(),
            kj::heap<HeadResponseStream>(expectedBodySize)
                .attach(kj::mv(statusTextCopy), kj::mv(headersCopy))});
        }).eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
        return kj::heap<kj::NullStream>();
      } else {
        auto pipe = newOneWayPipe(expectedBodySize);

        // Wrap the stream in a wrapper that delays the last read (the one that signals EOF)
        // until the service's request promise has finished.
        auto wrapper =
            kj::heap<DelayedEofInputStream>(kj::mv(pipe.in), task.attach(kj::addRef(*this)));

        fulfiller->fulfill({statusCode, statusTextCopy, headersCopy.get(),
          wrapper.attach(kj::mv(statusTextCopy), kj::mv(headersCopy))});
        return kj::mv(pipe.out);
      }
    }

    kj::Own<WebSocket> acceptWebSocket(const HttpHeaders& headers) override {
      KJ_FAIL_REQUIRE("a WebSocket was not requested");
    }

   private:
    kj::HttpMethod method;
    kj::Own<kj::PromiseFulfiller<HttpClient::Response>> fulfiller;
    kj::Promise<void> task = nullptr;
  };

  class DelayedCloseWebSocket final: public WebSocket {
    // A WebSocket wrapper that, when it reaches Close (in both directions), delays the final
    // close operation until some promise completes.

   public:
    DelayedCloseWebSocket(kj::Own<kj::WebSocket> inner, kj::Promise<void> completionTask)
        : inner(kj::mv(inner)),
          completionTask(kj::mv(completionTask)) {}

    kj::Promise<void> send(kj::ArrayPtr<const byte> message) override {
      return inner->send(message);
    }
    kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
      return inner->send(message);
    }
    kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
      co_await inner->close(code, reason);
      co_await afterSendClosed();
    }
    void disconnect() override {
      inner->disconnect();
    }
    void abort() override {
      // Don't need to worry about completion task in this case -- cancelling it is reasonable.
      inner->abort();
    }
    kj::Promise<void> whenAborted() override {
      return inner->whenAborted();
    }
    kj::Promise<Message> receive(size_t maxSize) override {
      auto message = co_await inner->receive(maxSize);
      if (message.is<WebSocket::Close>()) {
        co_await afterReceiveClosed();
      }
      co_return message;
    }
    kj::Promise<void> pumpTo(WebSocket& other) override {
      co_await inner->pumpTo(other);
      co_await afterReceiveClosed();
    }
    kj::Maybe<kj::Promise<void>> tryPumpFrom(WebSocket& other) override {
      return other.pumpTo(*inner).then([this]() { return afterSendClosed(); });
    }

    uint64_t sentByteCount() override {
      return inner->sentByteCount();
    }
    uint64_t receivedByteCount() override {
      return inner->receivedByteCount();
    }

    kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
      return inner->getPreferredExtensions(ctx);
    };

   private:
    kj::Own<kj::WebSocket> inner;
    kj::Maybe<kj::Promise<void>> completionTask;

    bool sentClose = false;
    bool receivedClose = false;

    kj::Promise<void> afterSendClosed() {
      sentClose = true;
      if (receivedClose) {
        KJ_IF_SOME(t, completionTask) {
          auto result = kj::mv(t);
          completionTask = kj::none;
          co_await result;
        }
      }
    }

    kj::Promise<void> afterReceiveClosed() {
      receivedClose = true;
      if (sentClose) {
        KJ_IF_SOME(t, completionTask) {
          auto result = kj::mv(t);
          completionTask = kj::none;
          co_await result;
        }
      }
    }
  };

  class WebSocketResponseImpl final: public HttpService::Response, public kj::Refcounted {
   public:
    WebSocketResponseImpl(kj::Own<kj::PromiseFulfiller<HttpClient::WebSocketResponse>> fulfiller)
        : fulfiller(kj::mv(fulfiller)) {}

    void setPromise(kj::Promise<void> promise) {
      task = promise.eagerlyEvaluate([this](kj::Exception&& exception) {
        if (fulfiller->isWaiting()) {
          fulfiller->reject(kj::mv(exception));
        } else {
          // We need to cause the client-side WebSocket to throw on close, so propagate the
          // exception.
          kj::throwRecoverableException(kj::mv(exception));
        }
      });
    }

    kj::Own<kj::AsyncOutputStream> send(uint statusCode,
        kj::StringPtr statusText,
        const HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
      // The caller of HttpClient is allowed to assume that the statusText and headers remain
      // valid until the body stream is dropped, but the HttpService implementation is allowed
      // to send values that are only valid until send() returns, so we have to copy.
      auto statusTextCopy = kj::str(statusText);
      auto headersCopy = kj::heap(headers.clone());

      if (expectedBodySize.orDefault(1) == 0) {
        // We're not expecting any body. We need to delay reporting completion to the client
        // until the server side has actually returned from the service method, otherwise we may
        // prematurely cancel it.

        task = task.then([this, statusCode, statusTextCopy = kj::mv(statusTextCopy),
                             headersCopy = kj::mv(headersCopy), expectedBodySize]() mutable {
          fulfiller->fulfill({statusCode, statusTextCopy, headersCopy.get(),
            kj::Own<AsyncInputStream>(kj::heap<HeadResponseStream>(expectedBodySize)
                                          .attach(kj::mv(statusTextCopy), kj::mv(headersCopy)))});
        }).eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
        return kj::heap<kj::NullStream>();
      } else {
        auto pipe = newOneWayPipe(expectedBodySize);

        // Wrap the stream in a wrapper that delays the last read (the one that signals EOF)
        // until the service's request promise has finished.
        kj::Own<AsyncInputStream> wrapper =
            kj::heap<DelayedEofInputStream>(kj::mv(pipe.in), task.attach(kj::addRef(*this)));

        fulfiller->fulfill({statusCode, statusTextCopy, headersCopy.get(),
          wrapper.attach(kj::mv(statusTextCopy), kj::mv(headersCopy))});
        return kj::mv(pipe.out);
      }
    }

    kj::Own<WebSocket> acceptWebSocket(const HttpHeaders& headers) override {
      // The caller of HttpClient is allowed to assume that the headers remain valid until the
      // body stream is dropped, but the HttpService implementation is allowed to send headers
      // that are only valid until acceptWebSocket() returns, so we have to copy.
      auto headersCopy = kj::heap(headers.clone());

      auto pipe = newWebSocketPipe();

      // Wrap the client-side WebSocket in a wrapper that delays clean close of the WebSocket
      // until the service's request promise has finished.
      kj::Own<WebSocket> wrapper =
          kj::heap<DelayedCloseWebSocket>(kj::mv(pipe.ends[0]), task.attach(kj::addRef(*this)));
      fulfiller->fulfill(
          {101, "Switching Protocols", headersCopy.get(), wrapper.attach(kj::mv(headersCopy))});
      return kj::mv(pipe.ends[1]);
    }

   private:
    kj::Own<kj::PromiseFulfiller<HttpClient::WebSocketResponse>> fulfiller;
    kj::Promise<void> task = nullptr;
  };

  class ConnectResponseImpl final: public HttpService::ConnectResponse, public kj::Refcounted {
   public:
    ConnectResponseImpl(kj::Own<kj::PromiseFulfiller<HttpClient::ConnectRequest::Status>> fulfiller,
        kj::Own<kj::AsyncIoStream> stream)
        : fulfiller(kj::mv(fulfiller)),
          streamAndFulfiller(initStreamsAndFulfiller(kj::mv(stream))) {}

    ~ConnectResponseImpl() noexcept(false) {
      if (fulfiller->isWaiting() || streamAndFulfiller.fulfiller->isWaiting()) {
        auto ex = KJ_EXCEPTION(
            FAILED, "service's connect() implementation never called accept() nor reject()");
        if (fulfiller->isWaiting()) {
          fulfiller->reject(ex.clone());
        }
        if (streamAndFulfiller.fulfiller->isWaiting()) {
          streamAndFulfiller.fulfiller->reject(kj::mv(ex));
        }
      }
    }

    void accept(uint statusCode, kj::StringPtr statusText, const HttpHeaders& headers) override {
      KJ_REQUIRE(statusCode >= 200 && statusCode < 300, "the statusCode must be 2xx for accept");
      respond(statusCode, statusText, headers);
    }

    kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
        kj::StringPtr statusText,
        const HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
      KJ_REQUIRE(
          statusCode < 200 || statusCode >= 300, "the statusCode must not be 2xx for reject.");
      auto pipe = kj::newOneWayPipe();
      respond(statusCode, statusText, headers, kj::mv(pipe.in));
      return kj::mv(pipe.out);
    }

   private:
    struct StreamsAndFulfiller {
      // `guarded` wraps the underlying stream but blocks writes until the connection is
      // accepted or rejected; it is handed off when getConnectStream() is called. The fulfiller
      // resolves (accept) or rejects (reject / exception) that write guard.
      kj::Own<kj::AsyncIoStream> guarded;
      kj::Own<kj::PromiseFulfiller<void>> fulfiller;
    };

    kj::Own<kj::PromiseFulfiller<HttpClient::ConnectRequest::Status>> fulfiller;
    StreamsAndFulfiller streamAndFulfiller;
    bool connectStreamDetached = false;

    StreamsAndFulfiller initStreamsAndFulfiller(kj::Own<kj::AsyncIoStream> stream) {
      auto paf = kj::newPromiseAndFulfiller<void>();
      auto guarded = kj::heap<GuardedAsyncIoStream>(
          kj::mv(stream), kj::READY_NOW /* read guard */, kj::mv(paf.promise) /* write guard */);
      return StreamsAndFulfiller{kj::mv(guarded), kj::mv(paf.fulfiller)};
    }

    void handleException(kj::Exception&& ex, kj::Own<kj::AsyncIoStream> connectStream) {
      // Reject the status promise if it is still pending...
      if (fulfiller->isWaiting()) {
        fulfiller->reject(ex.clone());
      }
      if (streamAndFulfiller.fulfiller->isWaiting()) {
        // If the guard hasn't yet been released, we can fail the pending reads by
        // rejecting the fulfiller here.
        streamAndFulfiller.fulfiller->reject(kj::mv(ex));
      } else {
        // The guard has already been released at this point.
        // TODO(connect) (inherited from kj): how to properly propagate the actual exception to
        // the connect stream? Here we "simply" shut it down.
        connectStream->abortRead();
        connectStream->shutdownWrite();
      }
    }

    kj::Own<kj::AsyncIoStream> getConnectStream() {
      KJ_ASSERT(!connectStreamDetached, "the connect stream was already detached");
      connectStreamDetached = true;
      return streamAndFulfiller.guarded.attach(kj::addRef(*this));
    }

    void respond(uint statusCode,
        kj::StringPtr statusText,
        const HttpHeaders& headers,
        kj::Maybe<kj::Own<kj::AsyncInputStream>> errorBody = kj::none) {
      if (errorBody == kj::none) {
        streamAndFulfiller.fulfiller->fulfill();
      } else {
        streamAndFulfiller.fulfiller->reject(
            KJ_EXCEPTION(DISCONNECTED, "the connect request was rejected"));
      }
      fulfiller->fulfill(HttpClient::ConnectRequest::Status(
          statusCode, kj::str(statusText), kj::heap(headers.clone()), kj::mv(errorBody)));
    }

    friend class ForeignServiceHttpClient;
  };
};

// kj's HttpServiceAdapter (in the unlinked kj-http-impl, not kj-http-types): the foreign path of
// kj::newHttpService(HttpClient&), for real C++ clients (e.g. server.c++'s cache client). A
// byte-for-byte behavioral port over public interfaces; the CONNECT read guard uses the
// GuardedAsyncIoStream replica.
class ForeignClientHttpService final: public HttpService {
 public:
  ForeignClientHttpService(HttpClient& client): client(client) {}

  kj::Promise<void> request(HttpMethod method,
      kj::StringPtr url,
      const HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    if (!headers.isWebSocket()) {
      auto innerReq = client.request(method, url, headers, requestBody.tryGetLength());

      auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);
      promises.add(requestBody.pumpTo(*innerReq.body)
                       .ignoreResult()
                       .attach(kj::mv(innerReq.body))
                       .eagerlyEvaluate(nullptr));

      promises.add(innerReq.response.then([&response](HttpClient::Response&& innerResponse) {
        auto out = response.send(innerResponse.statusCode, innerResponse.statusText,
            *innerResponse.headers, innerResponse.body->tryGetLength());
        auto promise = innerResponse.body->pumpTo(*out);
        return promise.ignoreResult().attach(kj::mv(out), kj::mv(innerResponse.body));
      }));

      return kj::joinPromisesFailFast(promises.finish());
    } else {
      return client.openWebSocket(url, headers)
          .then([&response](HttpClient::WebSocketResponse&& innerResponse) -> kj::Promise<void> {
        KJ_SWITCH_ONEOF(innerResponse.webSocketOrBody) {
          KJ_CASE_ONEOF(ws, kj::Own<WebSocket>) {
            auto ws2 = response.acceptWebSocket(*innerResponse.headers);
            auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);
            promises.add(ws->pumpTo(*ws2));
            promises.add(ws2->pumpTo(*ws));
            return kj::joinPromisesFailFast(promises.finish()).attach(kj::mv(ws), kj::mv(ws2));
          }
          KJ_CASE_ONEOF(body, kj::Own<kj::AsyncInputStream>) {
            auto out = response.send(innerResponse.statusCode, innerResponse.statusText,
                *innerResponse.headers, body->tryGetLength());
            auto promise = body->pumpTo(*out);
            return promise.ignoreResult().attach(kj::mv(out), kj::mv(body));
          }
        }
        KJ_UNREACHABLE;
      });
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      HttpConnectSettings settings) override {
    KJ_REQUIRE(!headers.isWebSocket(), "WebSocket upgrade headers are not permitted in a connect.");

    auto request = client.connect(host, headers, settings);

    // This operates optimistically. In order to support pipelining, we connect the
    // input and outputs streams immediately, even if we're not yet certain that the
    // tunnel can actually be established.
    auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);

    // For the inbound pipe (from the client's stream to the passed in stream)
    // we want to guard reads pending the acceptance of the tunnel. If the
    // tunnel is not accepted, the guard will be rejected, causing pending
    // reads to fail.
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto io = kj::heap<GuardedAsyncIoStream>(kj::mv(request.connection),
        kj::mv(paf.promise) /* read guard */, kj::READY_NOW /* write guard */);

    // Writing from connection to io is unguarded and allowed immediately.
    promises.add(connection.pumpTo(*io).then([&io = *io](uint64_t size) { io.shutdownWrite(); }));

    promises.add(
        io->pumpTo(connection).then([&connection](uint64_t size) { connection.shutdownWrite(); }));

    auto pumpPromise = kj::joinPromisesFailFast(promises.finish());

    return request.status
        .then([&response, &connection, fulfiller = kj::mv(paf.fulfiller),
                  pumpPromise = kj::mv(pumpPromise)](
                  HttpClient::ConnectRequest::Status status) mutable -> kj::Promise<void> {
      if (status.statusCode >= 200 && status.statusCode < 300) {
        // Release the read guard!
        fulfiller->fulfill();
        response.accept(status.statusCode, status.statusText, *status.headers);
        return kj::mv(pumpPromise);
      } else {
        // If the connect request is rejected, we want to shutdown the tunnel
        // and pipeline the status.errorBody to the AsyncOutputStream returned by
        // reject if it exists.
        pumpPromise = nullptr;
        connection.shutdownWrite();
        fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "the connect request was rejected"));
        KJ_IF_SOME(errorBody, status.errorBody) {
          auto out = response.reject(
              status.statusCode, status.statusText, *status.headers, errorBody->tryGetLength());
          return errorBody->pumpTo(*out)
              .then([](uint64_t) -> kj::Promise<void> {
            return kj::READY_NOW;
          }).attach(kj::mv(out), kj::mv(errorBody));
        } else {
          response.reject(status.statusCode, status.statusText, *status.headers, (uint64_t)0);
          return kj::READY_NOW;
        }
      }
    }).attach(kj::mv(io));
  }

 private:
  HttpClient& client;
};

}  // namespace

kj::Own<HttpClient> newHttpClient(HttpService& service) {
  KJ_IF_SOME(hyper,
      kj::dynamicDowncastIfAvailable<workerd::rust::kj_hyper::HyperHttpService>(service)) {
    // Our own hyper-backed service: the client shape is a new handle to the same Rust client
    // (shared upstream + keep-alive pool) — the composition stays inside Rust.
    return kj::heap<workerd::rust::kj_hyper::HyperHttpClient>(hyper.cloneClient());
  }
  return kj::heap<ForeignServiceHttpClient>(service);
}

kj::Own<HttpService> newHttpService(HttpClient& client) {
  KJ_IF_SOME(hyper,
      kj::dynamicDowncastIfAvailable<workerd::rust::kj_hyper::HyperHttpClient>(client)) {
    // Our own hyper-backed client: the service shape is a new handle to the same Rust client.
    return kj::heap<workerd::rust::kj_hyper::HyperHttpService>(hyper.cloneClient());
  }
  return kj::heap<ForeignClientHttpService>(client);
}

// =======================================================================================
// kj::HttpServer, hyper-backed.
//
// The class's data members (declared in kj/compat/http.h) are exactly the generic state this
// implementation needs: the timer/table/service refs, the drain fork + fulfiller, the
// connection count with its zero-connections fulfiller, and the TaskSet holding per-connection
// serve tasks. The suspend machinery (SuspendableRequest / the factory listenHttpCleanDrain
// overload) is not used by workerd and left undefined: a new call site fails to link rather
// than misbehaving.

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
  // Mirrors kj: per-connection failures go to the configured listen-loop handler, else the
  // default log line.
  KJ_IF_SOME(handler, settings.errorHandler) {
    handler.handleListenLoopException(kj::mv(exception));
  } else {
    KJ_LOG(ERROR, "unhandled exception in HTTP server", exception);
  }
}

kj::Promise<void> HttpServer::drain() {
  KJ_REQUIRE(!draining, "you can only call drain() once");
  draining = true;
  drainFulfiller->fulfill();

  if (connectionCount == 0) {
    return kj::READY_NOW;
  } else {
    auto paf = kj::newPromiseAndFulfiller<void>();
    zeroConnectionsFulfiller = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }
}

kj::Promise<bool> HttpServer::listenHttpImpl(kj::AsyncIoStream& connection, bool wantCleanDrain) {
  connectionCount++;
  KJ_DEFER({
    if (--connectionCount == 0) {
      KJ_IF_SOME(f, zeroConnectionsFulfiller) {
        f->fulfill();
      }
    }
  });

  // Per-connection service: the factory form makes one service per connection; the plain form
  // shares the one service.
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

  // The hyper inbound server serves the connection; drain() triggers its graceful shutdown
  // (idle connections close immediately, an in-flight request finishes with
  // "Connection: close"). The stream is handed over borrowed: the caller (or the enclosing
  // task's attachments) owns it for the duration of this promise. Application errors route
  // through the configured error handler, as in kj's Connection.
  ErrorHandlingService errorHandling(*servicePtr, settings);
  auto conn = workerd::rust::kj_hyper::newHyperHttpConnection(requestHeaderTable, errorHandling,
      fakeOwn(connection),
      // Workerd only ever installs JsgifyWebSocketErrors, so handler presence selects the
      // jsg.Error rendering of WebSocket protocol errors (same convention as jsgifyFor()).
      settings.webSocketErrorHandler != kj::none);
  auto& connRef = *conn;
  auto drainListener =
      onDrain.addBranch().then([&connRef]() { connRef.shutdown(); }).eagerlyEvaluate(nullptr);
  if (draining) connRef.shutdown();
  co_await connRef.serve();
  // Hyper's graceful shutdown always closes the connection (Connection: close), so a drained
  // connection is never reusable by a future server; kj's "clean drain" reusable=true case
  // does not arise.
  co_return false;
}

kj::Promise<void> HttpServer::listenHttp(kj::Own<kj::AsyncIoStream> connection) {
  auto promise = listenHttpImpl(*connection, /*wantCleanDrain=*/false).ignoreResult();
  return promise.attach(kj::mv(connection));
}

kj::Promise<bool> HttpServer::listenHttpCleanDrain(kj::AsyncIoStream& connection) {
  return listenHttpImpl(connection, /*wantCleanDrain=*/true);
}

kj::Promise<void> HttpServer::listenLoop(kj::ConnectionReceiver& port) {
  for (;;) {
    auto connection = co_await port.accept();
    auto& connRef = *connection;
    tasks.add(listenHttpImpl(connRef, /*wantCleanDrain=*/false)
                  .ignoreResult()
                  .attach(kj::mv(connection)));
  }
}

kj::Promise<void> HttpServer::listenHttp(kj::ConnectionReceiver& port) {
  return listenLoop(port);
}

// =======================================================================================
// The HTTP/1.1 text codec, rust-backed.
//
// Under the rust backend these codec symbols live in the unlinked kj-http-impl (kj/compat/http.c++,
// not the linked kj-http-types); this TU defines them over the byte-exact
// rust ports in kj-rs-http's codec.rs instead. The contract is exact parity with kj: the same
// output bytes (serialization), the same accept/reject decisions and in-place buffer mutations
// (parsing -- parsed names/values are NUL-terminated StringPtrs into the request buffer,
// exactly as kj's parseHeaders() leaves them), and the same KJ_REQUIRE texts (workerd's tests
// assert exact bytes and messages).

namespace {

// kj-http's method-name table, generated from KJ_HTTP_FOR_EACH_METHOD (kj/compat/http.h) like
// kj's own. Kept in C++: the macro is the single source of truth for the HttpMethod enum, so
// there is no rust duplicate to drift.
const char* METHOD_NAMES[] = {
#define METHOD_NAME(id) #id,
  KJ_HTTP_FOR_EACH_METHOD(METHOD_NAME)
#undef METHOD_NAME
};

// kj's requireValidRequestUrl/requireValidStatusText, over the rust validators. Named
// identically so the KJ_REQUIRE condition text -- part of the thrown exception's description --
// matches kj's byte for byte.
bool isValidRequestUrl(kj::StringPtr url) {
  return kj::rust::is_valid_request_url(url.asBytes().as<kj_rs::Rust>());
}

void requireValidRequestUrl(kj::StringPtr url) {
  KJ_REQUIRE(isValidRequestUrl(url), "invalid request URL", kj::encodeCEscape(url));
}

bool isValidStatusText(kj::StringPtr text) {
  return kj::rust::is_valid_status_text(text.asBytes().as<kj_rs::Rust>());
}

void requireValidStatusText(kj::StringPtr text) {
  KJ_REQUIRE(isValidStatusText(text), "invalid status text", kj::encodeCEscape(text));
}

}  // namespace

kj::StringPtr KJ_STRINGIFY(HttpMethod method) {
  auto index = static_cast<uint>(method);
  KJ_ASSERT(index < kj::size(METHOD_NAMES), "invalid HTTP method");

  return METHOD_NAMES[index];
}

kj::StringPtr KJ_STRINGIFY(HttpConnectMethod method) {
  return "CONNECT"_kj;
}

kj::Maybe<HttpMethod> tryParseHttpMethod(kj::StringPtr name) {
  KJ_IF_SOME(method, tryParseHttpMethodAllowingConnect(name)) {
    KJ_SWITCH_ONEOF(method) {
      KJ_CASE_ONEOF(m, HttpMethod) {
        return m;
      }
      KJ_CASE_ONEOF(m, HttpConnectMethod) {
        return kj::none;
      }
    }
    KJ_UNREACHABLE;
  } else {
    return kj::none;
  }
}

kj::Maybe<kj::OneOf<HttpMethod, HttpConnectMethod>> tryParseHttpMethodAllowingConnect(
    kj::StringPtr name) {
  // A linear scan over the macro-generated table rather than kj's hand-rolled character trie
  // (consumeHttpMethod, internal to kj-http's own TUs): the same exact-match, case-sensitive
  // semantics -- a method parses iff the whole string equals a table entry (or "CONNECT").
  if (name == "CONNECT"_kj) {
    return kj::Maybe<kj::OneOf<HttpMethod, HttpConnectMethod>>(HttpConnectMethod());
  }
  for (auto i: kj::indices(METHOD_NAMES)) {
    if (name == METHOD_NAMES[i]) {
      return kj::Maybe<kj::OneOf<HttpMethod, HttpConnectMethod>>(static_cast<HttpMethod>(i));
    }
  }
  return kj::none;
}

kj::String HttpHeaders::serializeRequest(HttpMethod method,
    kj::StringPtr url,
    kj::ArrayPtr<const kj::StringPtr> connectionHeaders) const {
  requireValidRequestUrl(url);
  return serialize(kj::toCharSequence(method), url, "HTTP/1.1"_kj, connectionHeaders);
}

kj::String HttpHeaders::serializeConnectRequest(
    kj::StringPtr authority, kj::ArrayPtr<const kj::StringPtr> connectionHeaders) const {
  requireValidRequestUrl(authority);
  return serialize("CONNECT"_kj, authority, "HTTP/1.1"_kj, connectionHeaders);
}

kj::String HttpHeaders::serializeResponse(uint statusCode,
    kj::StringPtr statusText,
    kj::ArrayPtr<const kj::StringPtr> connectionHeaders) const {
  requireValidStatusText(statusText);

  auto statusCodeStr = kj::toCharSequence(statusCode);

  return serialize("HTTP/1.1"_kj, statusCodeStr, statusText, connectionHeaders);
}

kj::String HttpHeaders::serialize(kj::ArrayPtr<const char> word1,
    kj::ArrayPtr<const char> word2,
    kj::ArrayPtr<const char> word3,
    kj::ArrayPtr<const kj::StringPtr> connectionHeaders) const {
  // kj's flattening, fed to the rust serializer pair by pair: the start line iff word1 is
  // non-null (the toString() form passes three nulls), then for each indexed slot the
  // connection-header override if one is provided (else the indexed value), skipping nulls,
  // then the unindexed headers. The rust side emits kj's exact bytes.
  KJ_ASSERT(connectionHeaders.size() <= indexedHeaders.size());
  auto serializer = kj::rust::new_http_serializer(word1.asBytes().as<kj_rs::Rust>(),
      word2.asBytes().as<kj_rs::Rust>(), word3.asBytes().as<kj_rs::Rust>());
  for (auto i: kj::indices(indexedHeaders)) {
    kj::StringPtr value = i < connectionHeaders.size() ? connectionHeaders[i] : indexedHeaders[i];
    if (value != nullptr) {
      serializer->add_header(table->idToString(HttpHeaderId(table, i)).asBytes().as<kj_rs::Rust>(),
          value.asBytes().as<kj_rs::Rust>());
    }
  }
  for (auto& header: unindexedHeaders) {
    serializer->add_header(
        header.name.asBytes().as<kj_rs::Rust>(), header.value.asBytes().as<kj_rs::Rust>());
  }

  auto bytes = serializer->finish();
  auto result = kj::heapString(bytes.size());
  memcpy(result.begin(), bytes.data(), bytes.size());
  return result;
}

kj::String HttpHeaders::toString() const {
  return serialize(nullptr, nullptr, nullptr, nullptr);
}

bool HttpHeaders::tryParse(kj::ArrayPtr<char> content) {
  // The rust parser performs kj's trimHeaderEnding itself (stripping the trailing "\r\n" and
  // writing the '\0' sentinel), then parses in place: each span is one header's name/value byte
  // ranges, NUL-terminated in-buffer, so the StringPtrs handed to addNoCheck() borrow the
  // request buffer exactly as kj's parseHeaders() ones do.
  auto result = kj::rust::parse_http_headers(content.asBytes().as<kj_rs::RustMutable>());
  if (!result.ok) return false;

  char* buf = content.begin();
  for (const auto& span: result.spans) {
    addNoCheck(kj::StringPtr(buf + span.name_begin, buf + span.name_end),
        kj::StringPtr(buf + span.value_begin, buf + span.value_end));
  }
  return true;
}

bool HttpHeaders::parseHeaders(char* ptr, char* end) {
  // kj's contract: the buffer was already trimmed by trimHeaderEnding(), so `end` points AT the
  // '\0' sentinel -- pass [ptr, end] inclusive of it. Spans as in tryParse() above.
  auto result = kj::rust::parse_http_headers_trimmed(
      ::rust::Slice<kj::byte>(reinterpret_cast<kj::byte*>(ptr), end - ptr + 1));
  if (!result.ok) return false;

  for (const auto& span: result.spans) {
    addNoCheck(kj::StringPtr(ptr + span.name_begin, ptr + span.name_end),
        kj::StringPtr(ptr + span.value_begin, ptr + span.value_end));
  }
  return true;
}

kj::String KJ_STRINGIFY(HttpByteRange range) {
  return kj::str(range.start, "-", range.end);
}

HttpRanges tryParseHttpRangeHeader(kj::ArrayPtr<const char> value, uint64_t contentLength) {
  auto result = kj::rust::parse_range_header(value.asBytes().as<kj_rs::Rust>(), contentLength);
  switch (result.kind) {
    case 0: {  // an array of satisfiable ranges
      auto ranges = kj::heapArrayBuilder<HttpByteRange>(result.ranges.size());
      for (const auto& range: result.ranges) {
        ranges.add(HttpByteRange{range.start, range.end});
      }
      return ranges.finish();
    }
    case 1:  // a range spec covered the full body
      return HttpEverythingRange{};
    default:  // invalid or nothing satisfiable
      return HttpUnsatisfiableRange{};
  }
}

// =======================================================================================
// kj::WebSocket interface defaults (kj evicted them into kj-http-impl with the codec: this TU
// is the class's home under the rust backend).

kj::Promise<void> WebSocket::pumpTo(WebSocket& other) {
  KJ_IF_SOME(p, other.tryPumpFrom(*this)) {
    // Yay, optimized pump!
    return kj::mv(p);
  } else {
    // Fall back to the default implementation: kj's pumpWebSocketLoop, ported to rust
    // (kj-hyper's websocket_default_pump -- receive from `this`, forward into `other`, a
    // forwarded Close completes the pump, any error disconnect()s `other` and propagates),
    // wrapped in kj's exact abort/cancellation race.
    return kj::evalNow([&]() {
      auto cancelPromise = other.whenAborted().then([this]() -> kj::Promise<void> {
        this->abort();
        return KJ_EXCEPTION(DISCONNECTED, "destination of WebSocket pump disconnected prematurely");
      });
      return workerd::rust::kj_hyper::websocket_default_pump(*this, other)
          .exclusiveJoin(kj::mv(cancelPromise));
    });
  }
}

kj::Maybe<kj::Promise<void>> WebSocket::tryPumpFrom(WebSocket& other) {
  return kj::none;
}

}  // namespace kj

#endif  // WORKERD_RUST_IO_BACKEND_RUST
