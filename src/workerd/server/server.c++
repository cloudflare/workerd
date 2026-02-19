// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "server.h"

#include "container-client.h"
#include "pyodide.h"
#include "workerd-api.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/analytics-engine.capnp.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/trace.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/actor-cache.h>
#include <workerd/io/actor-id.h>
#include <workerd/io/actor-sqlite.h>
#include <workerd/io/bundle-fs.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/container.capnp.h>
#include <workerd/io/hibernation-manager.h>
#include <workerd/io/io-context.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/io/request-tracker.h>
#include <workerd/io/trace-stream.h>
#include <workerd/io/worker-entrypoint.h>
#include <workerd/io/worker-fs.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/server/actor-id-impl.h>
#include <workerd/server/facet-tree-index.h>
#include <workerd/server/fallback-service.h>
#include <workerd/util/http-util.h>
#include <workerd/util/mimetype.h>
#include <workerd/util/use-perfetto-categories.h>
#include <workerd/util/uuid.h>
#include <workerd/util/websocket-error-handler.h>

#include <openssl/bio.h>
#include <openssl/pem.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <capnp/rpc-twoparty.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/compat/url.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/glob-filter.h>
#include <kj/map.h>

#include <cstdlib>
#include <ctime>

namespace workerd::server {

namespace {

struct PemData {
  kj::String type;
  kj::Array<byte> data;
};

// Decode PEM format using OpenSSL helpers.
static kj::Maybe<PemData> decodePem(kj::ArrayPtr<const char> text) {
  // TODO(cleanup): Should this be part of the KJ TLS library? We don't technically use it for TLS.
  //   Maybe KJ should have a general crypto library that wraps OpenSSL?

  BIO* bio = BIO_new_mem_buf(const_cast<char*>(text.begin()), text.size());
  KJ_DEFER(BIO_free(bio));

  class OpenSslDisposer: public kj::ArrayDisposer {
   public:
    void disposeImpl(void* firstElement,
        size_t elementSize,
        size_t elementCount,
        size_t capacity,
        void (*destroyElement)(void*)) const override {
      OPENSSL_free(firstElement);
    }
  };
  static constexpr OpenSslDisposer disposer;

  char* namePtr = nullptr;
  char* headerPtr = nullptr;
  byte* dataPtr = nullptr;
  long dataLen = 0;
  if (!PEM_read_bio(bio, &namePtr, &headerPtr, &dataPtr, &dataLen)) {
    return kj::none;
  }
  kj::Array<char> nameArr(namePtr, strlen(namePtr) + 1, disposer);
  KJ_DEFER(OPENSSL_free(headerPtr));
  kj::Array<kj::byte> data(dataPtr, dataLen, disposer);

  return PemData{kj::String(kj::mv(nameArr)), kj::mv(data)};
}

// Returns a time string in the format HTTP likes to use.
static kj::String httpTime(kj::Date date) {
  time_t time = (date - kj::UNIX_EPOCH) / kj::SECONDS;
#if _WIN32
  // `gmtime` is thread-safe on Windows: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/gmtime-gmtime32-gmtime64?view=msvc-170#return-value
  auto tm = *gmtime(&time);
#else
  struct tm tm;
  KJ_ASSERT(gmtime_r(&time, &tm) == &tm);
#endif
  char buf[256]{};
  size_t n = strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  KJ_ASSERT(n > 0);
  return kj::heapString(buf, n);
}

static kj::String escapeJsonString(kj::StringPtr text) {
  static const char HEXDIGITS[] = "0123456789abcdef";
  kj::Vector<char> escaped(text.size() + 1);

  for (char c: text) {
    switch (c) {
      case '"':
        escaped.addAll("\\\""_kj);
        break;
      case '\\':
        escaped.addAll("\\\\"_kj);
        break;
      case '\b':
        escaped.addAll("\\b"_kj);
        break;
      case '\f':
        escaped.addAll("\\f"_kj);
        break;
      case '\n':
        escaped.addAll("\\n"_kj);
        break;
      case '\r':
        escaped.addAll("\\r"_kj);
        break;
      case '\t':
        escaped.addAll("\\t"_kj);
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          escaped.addAll("\\u00"_kj);
          uint8_t c2 = c;
          escaped.add(HEXDIGITS[c2 / 16]);
          escaped.add(HEXDIGITS[c2 % 16]);
        } else {
          escaped.add(c);
        }
        break;
    }
  }

  return kj::str("\"", escaped.releaseAsArray(), "\"");
}

template <typename T>
static inline kj::Own<T> fakeOwn(T& ref) {
  return kj::Own<T>(&ref, kj::NullDisposer::instance);
}

void throwDynamicEntrypointTransferError() {
  JSG_FAIL_REQUIRE(DOMDataCloneError,
      "Entrypoints to dynamically-loaded workers cannot be transferred to other Workers, "
      "because the system does not know how to reload this Worker from scratch. Instead, "
      "have the parent Worker expose an entrypoint which constructs the dynamic worker "
      "and forwards to it.");
}

}  // namespace

// =======================================================================================

Server::Server(kj::Filesystem& fs,
    kj::Timer& timer,
    const kj::MonotonicClock& monotonicClock,
    kj::Network& network,
    kj::EntropySource& entropySource,
    Worker::LoggingOptions loggingOptions,
    kj::Function<void(kj::String)> reportConfigError)
    : fs(fs),
      timer(timer),
      monotonicClock(monotonicClock),
      network(network),
      entropySource(entropySource),
      reportConfigError(kj::mv(reportConfigError)),
      loggingOptions(loggingOptions),
      memoryCacheProvider(kj::heap<api::MemoryCacheProvider>(timer)),
      channelTokenHandler(*this),
      tasks(*this) {}

struct Server::GlobalContext {
  jsg::V8System& v8System;
  capnp::ByteStreamFactory byteStreamFactory;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;
  kj::HttpHeaderTable& headerTable;

  GlobalContext(
      Server& server, jsg::V8System& v8System, kj::HttpHeaderTable::Builder& headerTableBuilder)
      : v8System(v8System),
        httpOverCapnpFactory(
            byteStreamFactory, headerTableBuilder, capnp::HttpOverCapnpFactory::LEVEL_2),
        threadContext(server.timer,
            server.entropySource,
            headerTableBuilder,
            httpOverCapnpFactory,
            byteStreamFactory,
            false /* isFiddle -- TODO(beta): support */),
        headerTable(headerTableBuilder.getFutureTable()) {}
};

class Server::Service: public IoChannelFactory::SubrequestChannel {
 public:
  // Cross-links this service with other services. Must be called once before `startRequest()`.
  virtual void link(Worker::ValidationErrorReporter& errorReporter) {}

  // Drops any cross-links created during link(). This called just before all the services are
  // destroyed. An `Own<T>` cannot be destroyed unless the object it points to still exists, so
  // we must clear all the `Own<Service>`s before we can actually destroy the `Service`s.
  virtual void unlink() {}

  // Begin an incoming request. Returns a `WorkerInterface` object that will be used for one
  // request then discarded.
  virtual kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata) override = 0;

  // Returns true if the service exports the given handler, e.g. `fetch`, `scheduled`, etc.
  virtual bool hasHandler(kj::StringPtr handlerName) = 0;

  // Return the service itself, or the underlying service if this instance wraps another service as
  // with EntrypointService.
  virtual Service* service() {
    return this;
  }

  // Implemented by EntrypointService for loopback ctx.exports entrypoints, to allow props to be
  // specified.
  virtual kj::Own<Service> forProps(Frankenvalue props) {
    KJ_FAIL_REQUIRE("can't override props for this service");
  }

  void requireAllowsTransfer() override {
    // We consider all `Service` implementations to be safe to transfer, except for dynamic workers
    // which we'll handle explicitly.
  }
};

class Server::ActorClass: public IoChannelFactory::ActorClassChannel {
 public:
  // The caller must call this before calling newActor(). If it returns a promise, then the
  // caller must await the promise before calling other methods.
  //
  // In particular, this is needed with dynamically-loaded workers. The isolate may still be
  // loading when the caller calls `getDurableObjectClass()` on it.
  virtual kj::Maybe<kj::Promise<void>> whenReady() {
    return kj::none;
  }

  // Construct a new instance of the class. The parameters here are passed into `Worker::Actor`'s
  // constructor.
  virtual kj::Own<Worker::Actor> newActor(kj::Maybe<RequestTracker&> tracker,
      Worker::Actor::Id actorId,
      Worker::Actor::MakeActorCacheFunc makeActorCache,
      Worker::Actor::MakeStorageFunc makeStorage,
      kj::Own<Worker::Actor::Loopback> loopback,
      kj::Maybe<kj::Own<Worker::Actor::HibernationManager>> manager,
      kj::Maybe<rpc::Container::Client> container,
      kj::Maybe<Worker::Actor::FacetManager&> facetManager) = 0;

  // Start a request on the actor. (The actor must have been created using newActor().)
  virtual kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata, kj::Own<Worker::Actor> actor) = 0;

  virtual kj::Own<ActorClass> forProps(Frankenvalue props) {
    KJ_FAIL_REQUIRE("can't override props for this actor class");
  }
};

Server::~Server() noexcept {
  // This destructor is explicitly `noexcept` because if one of the `unlink()`s throws then we'd
  // have a hard time avoiding a segfault later... and we're shutting down the server anyway so
  // whatever, better to crash.

  // It's important to cancel all tasks before we start tearing down.
  tasks.clear();

  // Unlink all the services, which should remove all refcount cycles.
  unlinkWorkerLoaders();
  for (auto& service: services) {
    service.value->unlink();
  }

  // Verify that unlinking actually eliminated cycles. Otherwise we have a memory leak -- and
  // potentially use-after-free if we allow the `Server` to be destroyed while services still
  // exist.
  for (auto& service: services) {
    KJ_ASSERT(
        !service.value->isShared(), "service still has references after unlinking", service.key);
  }
}

// =======================================================================================

kj::Own<kj::TlsContext> Server::makeTlsContext(config::TlsOptions::Reader conf) {
  kj::TlsContext::Options options;

  struct Attachments {
    kj::Maybe<kj::TlsKeypair> keypair;
    kj::Array<kj::TlsCertificate> trustedCerts;
  };
  auto attachments = kj::heap<Attachments>();

  if (conf.hasKeypair()) {
    auto pairConf = conf.getKeypair();
    options.defaultKeypair = attachments->keypair.emplace(
        kj::TlsKeypair{.privateKey = kj::TlsPrivateKey(pairConf.getPrivateKey()),
          .certificate = kj::TlsCertificate(pairConf.getCertificateChain())});
  }

  options.verifyClients = conf.getRequireClientCerts();
  options.useSystemTrustStore = conf.getTrustBrowserCas();

  auto trustList = conf.getTrustedCertificates();
  if (trustList.size() > 0) {
    attachments->trustedCerts = KJ_MAP(cert, trustList) { return kj::TlsCertificate(cert); };
    options.trustedCertificates = attachments->trustedCerts;
  }

  switch (conf.getMinVersion()) {
    case config::TlsOptions::Version::GOOD_DEFAULT:
      // Don't change.
      goto validVersion;
    case config::TlsOptions::Version::SSL3:
      options.minVersion = kj::TlsVersion::SSL_3;
      goto validVersion;
    case config::TlsOptions::Version::TLS1_DOT0:
      options.minVersion = kj::TlsVersion::TLS_1_0;
      goto validVersion;
    case config::TlsOptions::Version::TLS1_DOT1:
      options.minVersion = kj::TlsVersion::TLS_1_1;
      goto validVersion;
    case config::TlsOptions::Version::TLS1_DOT2:
      options.minVersion = kj::TlsVersion::TLS_1_2;
      goto validVersion;
    case config::TlsOptions::Version::TLS1_DOT3:
      options.minVersion = kj::TlsVersion::TLS_1_3;
      goto validVersion;
  }
  reportConfigError(kj::str("Encountered unknown TlsOptions::minVersion setting. Was the "
                            "config compiled with a newer version of the schema?"));

validVersion:
  if (conf.hasCipherList()) {
    options.cipherList = conf.getCipherList();
  }

  return kj::heap<kj::TlsContext>(kj::mv(options));
}

kj::Promise<kj::Own<kj::NetworkAddress>> Server::makeTlsNetworkAddress(
    config::TlsOptions::Reader conf,
    kj::StringPtr addrStr,
    kj::Maybe<kj::StringPtr> certificateHost,
    uint defaultPort) {
  auto context = makeTlsContext(conf);

  KJ_IF_SOME(h, certificateHost) {
    auto parsed = co_await network.parseAddress(addrStr, defaultPort);
    co_return context->wrapAddress(kj::mv(parsed), h).attach(kj::mv(context));
  }

  // Wrap the `Network` itself so we can use the TLS implementation's `parseAddress()` to extract
  // the authority from the address.
  auto tlsNetwork = context->wrapNetwork(network);
  auto parsed = co_await network.parseAddress(addrStr, defaultPort);
  co_return parsed.attach(kj::mv(context));
}

// =======================================================================================

// Helper to apply config::HttpOptions.
class Server::HttpRewriter {
  // TODO(beta): Do we want to automatically add `Date`, `Server` (to outgoing responses),
  //   `User-Agent` (to outgoing requests), etc.?

 public:
  HttpRewriter(
      config::HttpOptions::Reader httpOptions, kj::HttpHeaderTable::Builder& headerTableBuilder)
      : style(httpOptions.getStyle()),
        requestInjector(httpOptions.getInjectRequestHeaders(), headerTableBuilder),
        responseInjector(httpOptions.getInjectResponseHeaders(), headerTableBuilder) {
    if (httpOptions.hasForwardedProtoHeader()) {
      forwardedProtoHeader = headerTableBuilder.add(httpOptions.getForwardedProtoHeader());
    }
    if (httpOptions.hasCfBlobHeader()) {
      cfBlobHeader = headerTableBuilder.add(httpOptions.getCfBlobHeader());
    }
    if (httpOptions.hasCapnpConnectHost()) {
      capnpConnectHost = httpOptions.getCapnpConnectHost();
    }
  }

  bool hasCfBlobHeader() {
    return cfBlobHeader != kj::none;
  }

  bool needsRewriteRequest() {
    return style == config::HttpOptions::Style::HOST || hasCfBlobHeader() ||
        !requestInjector.empty();
  }

  // Attach this to the promise returned by request().
  struct Rewritten {
    kj::Own<kj::HttpHeaders> headers;
    kj::String ownUrl;
  };

  Rewritten rewriteOutgoingRequest(
      kj::StringPtr& url, const kj::HttpHeaders& headers, kj::Maybe<kj::StringPtr> cfBlobJson) {
    Rewritten result{kj::heap(headers.cloneShallow()), nullptr};

    if (style == config::HttpOptions::Style::HOST) {
      auto parsed = kj::Url::parse(url, kj::Url::HTTP_PROXY_REQUEST,
          kj::Url::Options{.percentDecode = false, .allowEmpty = true});
      result.headers->set(kj::HttpHeaderId::HOST, kj::mv(parsed.host));
      KJ_IF_SOME(h, forwardedProtoHeader) {
        result.headers->set(h, kj::mv(parsed.scheme));
      }
      url = result.ownUrl = parsed.toString(kj::Url::HTTP_REQUEST);
    }

    KJ_IF_SOME(h, cfBlobHeader) {
      KJ_IF_SOME(b, cfBlobJson) {
        result.headers->setPtr(h, b);
      } else {
        result.headers->unset(h);
      }
    }

    requestInjector.apply(*result.headers);

    return result;
  }

  kj::Maybe<Rewritten> rewriteIncomingRequest(kj::StringPtr& url,
      kj::StringPtr physicalProtocol,
      const kj::HttpHeaders& headers,
      kj::Maybe<kj::String>& cfBlobJson) {
    Rewritten result{kj::heap(headers.cloneShallow()), nullptr};

    if (style == config::HttpOptions::Style::HOST) {
      auto parsed = kj::Url::parse(
          url, kj::Url::HTTP_REQUEST, kj::Url::Options{.percentDecode = false, .allowEmpty = true});
      parsed.host = kj::str(KJ_UNWRAP_OR_RETURN(headers.get(kj::HttpHeaderId::HOST), kj::none));

      KJ_IF_SOME(h, forwardedProtoHeader) {
        KJ_IF_SOME(s, headers.get(h)) {
          parsed.scheme = kj::str(s);
          result.headers->unset(h);
        }
      }

      if (parsed.scheme == nullptr) parsed.scheme = kj::str(physicalProtocol);

      url = result.ownUrl = parsed.toString(kj::Url::HTTP_PROXY_REQUEST);
    }

    KJ_IF_SOME(h, cfBlobHeader) {
      KJ_IF_SOME(b, headers.get(h)) {
        cfBlobJson = kj::str(b);
        result.headers->unset(h);
      }
    }

    requestInjector.apply(*result.headers);

    return result;
  }

  bool needsRewriteResponse() {
    return !responseInjector.empty();
  }

  void rewriteResponse(kj::HttpHeaders& headers) {
    responseInjector.apply(headers);
  }

  kj::Maybe<kj::StringPtr> getCapnpConnectHost() {
    return capnpConnectHost;
  }

 private:
  config::HttpOptions::Style style;
  kj::Maybe<kj::HttpHeaderId> forwardedProtoHeader;
  kj::Maybe<kj::HttpHeaderId> cfBlobHeader;
  kj::Maybe<kj::StringPtr> capnpConnectHost;

  class HeaderInjector {
   public:
    HeaderInjector(capnp::List<config::HttpOptions::Header>::Reader headers,
        kj::HttpHeaderTable::Builder& headerTableBuilder)
        : injectedHeaders(KJ_MAP(header, headers) {
            InjectedHeader result;
            result.id = headerTableBuilder.add(header.getName());
            if (header.hasValue()) {
              result.value = kj::str(header.getValue());
            }
            return result;
          }) {}

    bool empty() {
      return injectedHeaders.size() == 0;
    }

    void apply(kj::HttpHeaders& headers) {
      for (auto& header: injectedHeaders) {
        KJ_IF_SOME(v, header.value) {
          headers.setPtr(header.id, v);
        } else {
          headers.unset(header.id);
        }
      }
    }

   private:
    struct InjectedHeader {
      kj::HttpHeaderId id;
      kj::Maybe<kj::String> value;
    };
    kj::Array<InjectedHeader> injectedHeaders;
  };

  HeaderInjector requestInjector;
  HeaderInjector responseInjector;
};

// =======================================================================================

// Service used when the service's config is invalid.
class Server::InvalidConfigService final: public Service {
 public:
  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    JSG_FAIL_REQUIRE(Error, "Service cannot handle requests because its config is invalid.");
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    return false;
  }
};

class Server::InvalidConfigActorClass final: public ActorClass {
 public:
  void requireAllowsTransfer() override {
    // Can't get here because workerd would have failed to start.
    KJ_UNREACHABLE;
  }

  kj::Own<Worker::Actor> newActor(kj::Maybe<RequestTracker&> tracker,
      Worker::Actor::Id actorId,
      Worker::Actor::MakeActorCacheFunc makeActorCache,
      Worker::Actor::MakeStorageFunc makeStorage,
      kj::Own<Worker::Actor::Loopback> loopback,
      kj::Maybe<kj::Own<Worker::Actor::HibernationManager>> manager,
      kj::Maybe<rpc::Container::Client> container,
      kj::Maybe<Worker::Actor::FacetManager&> facetManager) override {
    JSG_FAIL_REQUIRE(
        Error, "Cannot instantiate Durable Object class because its config is invalid.");
  }

  kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata, kj::Own<Worker::Actor> actor) override {
    // Can't get here because creating the actor would have required calling the other method.
    KJ_UNREACHABLE;
  }
};

// Return a fake Own pointing to the singleton.
kj::Own<Server::Service> Server::makeInvalidConfigService() {
  return {invalidConfigServiceSingleton.get(), kj::NullDisposer::instance};
}

// A NetworkAddress whose connect() method waits for a Promise<NetworkAddress> and then forwards
// to it. Used by ExternalHttpService so that we don't have to wait for DNS lookup before the
// server can start.
class PromisedNetworkAddress final: public kj::NetworkAddress {
  // TODO(cleanup): kj::Network should be extended with a new version of parseAddress() which does
  //   not do DNS lookup immediately, and therefore can return a NetworkAddress synchronously.
  //   In fact, this version should be designed to redo the DNS lookup periodically to see if it
  //   changed, which would be nice for workerd when the remote address may change over time.
 public:
  PromisedNetworkAddress(kj::Promise<kj::Own<kj::NetworkAddress>> promise)
      : promise(promise.then([this](kj::Own<kj::NetworkAddress> result) { addr = kj::mv(result); })
                    .fork()) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
    KJ_IF_SOME(a, addr) {
      co_return co_await a.get()->connect();
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(addr)->connect();
    }
  }

  kj::Promise<kj::AuthenticatedStream> connectAuthenticated() override {
    KJ_IF_SOME(a, addr) {
      co_return co_await a.get()->connectAuthenticated();
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(addr)->connectAuthenticated();
    }
  }

  // We don't use any other methods, and they seem kinda annoying to implement.
  kj::Own<kj::ConnectionReceiver> listen() override {
    KJ_UNIMPLEMENTED("PromisedNetworkAddress::listen() not implemented");
  }
  kj::Own<kj::NetworkAddress> clone() override {
    KJ_UNIMPLEMENTED("PromisedNetworkAddress::clone() not implemented");
  }
  kj::String toString() override {
    KJ_UNIMPLEMENTED("PromisedNetworkAddress::toString() not implemented");
  }

 private:
  kj::ForkedPromise<void> promise;
  kj::Maybe<kj::Own<kj::NetworkAddress>> addr;
};

class Server::ExternalTcpService final: public Service, private WorkerInterface {
 public:
  ExternalTcpService(kj::Own<kj::NetworkAddress> addrParam): addr(kj::mv(addrParam)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return {this, kj::NullDisposer::instance};
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    return handlerName == "fetch"_kj || handlerName == "connect"_kj;
  }

 private:
  kj::Own<kj::NetworkAddress> addr;

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    throwUnsupported();
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& tunnel,
      kj::HttpConnectSettings settings) override {
    TRACE_EVENT("workerd", "ExternalTcpService::connect()", "host", host.cStr());
    auto io_stream = co_await addr->connect();

    auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);

    promises.add(connection.pumpTo(*io_stream).then([&io_stream = *io_stream](uint64_t size) {
      io_stream.shutdownWrite();
    }));

    promises.add(io_stream->pumpTo(connection).then([&connection](uint64_t size) {
      connection.shutdownWrite();
    }));

    tunnel.accept(200, "OK", kj::HttpHeaders(kj::HttpHeaderTable{}));

    co_await kj::joinPromisesFailFast(promises.finish()).attach(kj::mv(io_stream));
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    return kj::READY_NOW;
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    throwUnsupported();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    throwUnsupported();
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

  [[noreturn]] void throwUnsupported() {
    JSG_FAIL_REQUIRE(Error, "External TCP servers don't support this event type.");
  }
};

// Service used when the service is configured as external HTTP service.
class Server::ExternalHttpService final: public Service {
 public:
  ExternalHttpService(kj::Own<kj::NetworkAddress> addrParam,
      kj::Own<HttpRewriter> rewriter,
      kj::HttpHeaderTable& headerTable,
      kj::Timer& timer,
      kj::EntropySource& entropySource,
      capnp::ByteStreamFactory& byteStreamFactory,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
      : addr(kj::mv(addrParam)),
        webSocketErrorHandler(kj::heap<JsgifyWebSocketErrors>()),
        inner(kj::newHttpClient(timer,
            headerTable,
            *addr,
            {.entropySource = entropySource,
              .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION,
              .webSocketErrorHandler = *webSocketErrorHandler})),
        serviceAdapter(kj::newHttpService(*inner)),
        rewriter(kj::mv(rewriter)),
        headerTable(headerTable),
        byteStreamFactory(byteStreamFactory),
        httpOverCapnpFactory(httpOverCapnpFactory) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return kj::heap<WorkerInterfaceImpl>(*this, kj::mv(metadata));
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    return handlerName == "fetch"_kj || handlerName == "connect"_kj;
  }

 private:
  kj::Own<kj::NetworkAddress> addr;

  kj::Own<JsgifyWebSocketErrors> webSocketErrorHandler;
  kj::Own<kj::HttpClient> inner;
  kj::Own<kj::HttpService> serviceAdapter;

  kj::Own<HttpRewriter> rewriter;

  kj::HttpHeaderTable& headerTable;
  capnp::ByteStreamFactory& byteStreamFactory;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;

  struct CapnpClient {
    kj::Own<kj::AsyncIoStream> connection;
    capnp::TwoPartyClient rpcSystem;

    CapnpClient(kj::Own<kj::AsyncIoStream> connectionParam)
        : connection(kj::mv(connectionParam)),
          rpcSystem(*connection) {}
  };

  // capnpClient is created on-demand when RPC is needed.
  kj::Maybe<CapnpClient> capnpClient;

  // This task nulls out `capnpClient` when the connection is lost.
  kj::Promise<void> clearCapnpClientTask = nullptr;

  // Get an WorkerdBootstrap representing the service on the other end of an HTTP connection. May
  // reuse an existing connection, or form a new one over `client`.
  rpc::WorkerdBootstrap::Client getOutgoingCapnp(kj::HttpClient& client) {
    KJ_IF_SOME(c, capnpClient) {
      return c.rpcSystem.bootstrap().castAs<rpc::WorkerdBootstrap>();
    }

    // No existing client, need to create a new one.
    kj::StringPtr host = KJ_UNWRAP_OR(rewriter->getCapnpConnectHost(),
        { return JSG_KJ_EXCEPTION(FAILED, Error, "This ExternalServer not configured for RPC."); });

    auto req = client.connect(host, kj::HttpHeaders(headerTable), {});
    auto& c = capnpClient.emplace(kj::mv(req.connection));

    // Arrange that when the connection is lost, we'll null out `capnpClient`. This ensures that
    // on the next event, we'll attempt to reconnect.
    //
    // TODO(perf): Time out idle connections?
    clearCapnpClientTask =
        c.rpcSystem.onDisconnect().attach(kj::defer([this]() {
      capnpClient = kj::none;
    })).eagerlyEvaluate(nullptr);

    return c.rpcSystem.bootstrap().castAs<rpc::WorkerdBootstrap>();
  }

  class WorkerInterfaceImpl final: public WorkerInterface, private kj::HttpService::Response {
   public:
    WorkerInterfaceImpl(ExternalHttpService& parent, IoChannelFactory::SubrequestMetadata metadata)
        : parent(kj::addRef(parent)),
          metadata(kj::mv(metadata)) {}

    kj::Promise<void> request(kj::HttpMethod method,
        kj::StringPtr url,
        const kj::HttpHeaders& headers,
        kj::AsyncInputStream& requestBody,
        kj::HttpService::Response& response) override {
      TRACE_EVENT("workerd", "ExternalHttpServer::request()");
      KJ_REQUIRE(wrappedResponse == kj::none, "object should only receive one request");
      wrappedResponse = response;
      if (parent->rewriter->needsRewriteRequest()) {
        auto rewrite = parent->rewriter->rewriteOutgoingRequest(url, headers, metadata.cfBlobJson);
        return parent->serviceAdapter->request(method, url, *rewrite.headers, requestBody, *this)
            .attach(kj::mv(rewrite));
      } else {
        return parent->serviceAdapter->request(method, url, headers, requestBody, *this);
      }
    }

    kj::Promise<void> connect(kj::StringPtr host,
        const kj::HttpHeaders& headers,
        kj::AsyncIoStream& connection,
        ConnectResponse& tunnel,
        kj::HttpConnectSettings settings) override {
      TRACE_EVENT("workerd", "ExternalHttpServer::connect()");
      return parent->serviceAdapter->connect(host, headers, connection, tunnel, kj::mv(settings));
    }

    kj::Promise<void> prewarm(kj::StringPtr url) override {
      return kj::READY_NOW;
    }
    kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
      throwUnsupported();
    }
    kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
      throwUnsupported();
    }

    kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
      // We'll use capnp RPC for custom events.
      auto bootstrap = parent->getOutgoingCapnp(*parent->inner);
      auto dispatcher =
          bootstrap.startEventRequest(capnp::MessageSize{4, 0}).send().getDispatcher();
      return event
          ->sendRpc(parent->httpOverCapnpFactory, parent->byteStreamFactory, kj::mv(dispatcher))
          .attach(kj::mv(event));
    }

   private:
    kj::Own<ExternalHttpService> parent;
    IoChannelFactory::SubrequestMetadata metadata;
    kj::Maybe<kj::HttpService::Response&> wrappedResponse;

    [[noreturn]] void throwUnsupported() {
      JSG_FAIL_REQUIRE(Error, "External HTTP servers don't support this event type.");
    }

    kj::Own<kj::AsyncOutputStream> send(uint statusCode,
        kj::StringPtr statusText,
        const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize) override {
      TRACE_EVENT("workerd", "ExternalHttpService::send()", "status", statusCode);
      auto& response = KJ_ASSERT_NONNULL(wrappedResponse);
      if (parent->rewriter->needsRewriteResponse()) {
        auto rewrite = headers.cloneShallow();
        parent->rewriter->rewriteResponse(rewrite);
        return response.send(statusCode, statusText, rewrite, expectedBodySize);
      } else {
        return response.send(statusCode, statusText, headers, expectedBodySize);
      }
    }

    kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
      TRACE_EVENT("workerd", "ExternalHttpService::acceptWebSocket()");
      auto& response = KJ_ASSERT_NONNULL(wrappedResponse);
      if (parent->rewriter->needsRewriteResponse()) {
        auto rewrite = headers.cloneShallow();
        parent->rewriter->rewriteResponse(rewrite);
        return response.acceptWebSocket(rewrite);
      } else {
        return response.acceptWebSocket(headers);
      }
    }
  };
};

kj::Own<Server::Service> Server::makeExternalService(kj::StringPtr name,
    config::ExternalServer::Reader conf,
    kj::HttpHeaderTable::Builder& headerTableBuilder) {
  TRACE_EVENT("workerd", "Server::makeExternalService()", "name", name.cStr());
  kj::StringPtr addrStr = nullptr;
  kj::String ownAddrStr = nullptr;

  KJ_IF_SOME(override, externalOverrides.findEntry(name)) {
    addrStr = ownAddrStr = kj::mv(override.value);
    externalOverrides.erase(override);
  } else if (conf.hasAddress()) {
    addrStr = conf.getAddress();
  } else {
    reportConfigError(kj::str("External service \"", name,
        "\" has no address in the config, so must be specified "
        "on the command line with `--external-addr`."));
    return makeInvalidConfigService();
  }

  switch (conf.which()) {
    case config::ExternalServer::HTTP: {
      // We have to construct the rewriter upfront before waiting on any promises, since the
      // HeaderTable::Builder is only available synchronously.
      auto rewriter = kj::heap<HttpRewriter>(conf.getHttp(), headerTableBuilder);
      auto addr = kj::heap<PromisedNetworkAddress>(network.parseAddress(addrStr, 80));
      return kj::refcounted<ExternalHttpService>(kj::mv(addr), kj::mv(rewriter),
          headerTableBuilder.getFutureTable(), timer, entropySource,
          globalContext->byteStreamFactory, globalContext->httpOverCapnpFactory);
    }
    case config::ExternalServer::HTTPS: {
      auto httpsConf = conf.getHttps();
      kj::Maybe<kj::StringPtr> certificateHost;
      if (httpsConf.hasCertificateHost()) {
        certificateHost = httpsConf.getCertificateHost();
      }
      auto rewriter = kj::heap<HttpRewriter>(httpsConf.getOptions(), headerTableBuilder);
      auto addr = kj::heap<PromisedNetworkAddress>(
          makeTlsNetworkAddress(httpsConf.getTlsOptions(), addrStr, certificateHost, 443));
      return kj::refcounted<ExternalHttpService>(kj::mv(addr), kj::mv(rewriter),
          headerTableBuilder.getFutureTable(), timer, entropySource,
          globalContext->byteStreamFactory, globalContext->httpOverCapnpFactory);
    }
    case config::ExternalServer::TCP: {
      auto tcpConf = conf.getTcp();
      auto addr = kj::heap<PromisedNetworkAddress>(network.parseAddress(addrStr, 80));
      if (tcpConf.hasTlsOptions()) {
        kj::Maybe<kj::StringPtr> certificateHost;
        if (tcpConf.hasCertificateHost()) {
          certificateHost = tcpConf.getCertificateHost();
        }
        addr = kj::heap<PromisedNetworkAddress>(
            makeTlsNetworkAddress(tcpConf.getTlsOptions(), addrStr, certificateHost, 0));
      }
      return kj::refcounted<ExternalTcpService>(kj::mv(addr));
    }
  }
  reportConfigError(kj::str("External service named \"", name,
      "\" has unrecognized protocol. Was the config "
      "compiled with a newer version of the schema?"));
  return makeInvalidConfigService();
}

// Service used when the service is configured as network service.
class Server::NetworkService final: public Service, private WorkerInterface {
 public:
  NetworkService(kj::HttpHeaderTable& headerTable,
      kj::Timer& timer,
      kj::EntropySource& entropySource,
      kj::Own<kj::Network> networkParam,
      kj::Maybe<kj::Own<kj::Network>> tlsNetworkParam,
      kj::Maybe<kj::SecureNetworkWrapper&> tlsContext)
      : network(kj::mv(networkParam)),
        tlsNetwork(kj::mv(tlsNetworkParam)),
        webSocketErrorHandler(kj::heap<JsgifyWebSocketErrors>()),
        inner(kj::newHttpClient(timer,
            headerTable,
            *network,
            tlsNetwork,
            {.entropySource = entropySource,
              .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION,
              .webSocketErrorHandler = *webSocketErrorHandler,
              .tlsContext = tlsContext})),
        serviceAdapter(kj::newHttpService(*inner)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return {this, kj::NullDisposer::instance};
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    return handlerName == "fetch"_kj || handlerName == "connect"_kj;
  }

 private:
  kj::Own<kj::Network> network;
  kj::Maybe<kj::Own<kj::Network>> tlsNetwork;
  kj::Own<JsgifyWebSocketErrors> webSocketErrorHandler;
  kj::Own<kj::HttpClient> inner;
  kj::Own<kj::HttpService> serviceAdapter;

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    TRACE_EVENT("workerd", "NetworkService::request()");
    return serviceAdapter->request(method, url, headers, requestBody, response);
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& tunnel,
      kj::HttpConnectSettings settings) override {
    TRACE_EVENT("workerd", "NetworkService::connect()");
    // This code is hit when the global `connect` function is called in a JS worker script.
    // It represents a proxy-less TCP connection, which means we can simply defer the handling of
    // the connection to the service adapter (likely NetworkHttpClient). Its behavior will be to
    // connect directly to the host over TCP.
    return serviceAdapter->connect(host, headers, connection, tunnel, kj::mv(settings));
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    return kj::READY_NOW;
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    throwUnsupported();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    throwUnsupported();
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

  [[noreturn]] void throwUnsupported() {
    JSG_FAIL_REQUIRE(Error, "External HTTP servers don't support this event type.");
  }
};

kj::Own<Server::Service> Server::makeNetworkService(config::Network::Reader conf) {
  TRACE_EVENT("workerd", "Server::makeNetworkService()");
  auto restrictedNetwork = network.restrictPeers( KJ_MAP(a, conf.getAllow()) -> kj::StringPtr {
    return a;
  }, KJ_MAP(a, conf.getDeny()) -> kj::StringPtr { return a; });

  kj::Maybe<kj::Own<kj::Network>> tlsNetwork;
  kj::Maybe<kj::SecureNetworkWrapper&> tlsContext;
  if (conf.hasTlsOptions()) {
    auto ownedTlsContext = makeTlsContext(conf.getTlsOptions());
    tlsContext = ownedTlsContext;
    tlsNetwork = ownedTlsContext->wrapNetwork(*restrictedNetwork).attach(kj::mv(ownedTlsContext));
  }

  return kj::refcounted<NetworkService>(globalContext->headerTable, timer, entropySource,
      kj::mv(restrictedNetwork), kj::mv(tlsNetwork), tlsContext);
}

// Service used when the service is configured as disk directory service.
class Server::DiskDirectoryService final: public Service, private WorkerInterface {
 public:
  DiskDirectoryService(config::DiskDirectory::Reader conf,
      kj::Own<const kj::Directory> dir,
      kj::HttpHeaderTable::Builder& headerTableBuilder)
      : writable(*dir),
        readable(kj::mv(dir)),
        headerTable(headerTableBuilder.getFutureTable()),
        hLastModified(headerTableBuilder.add("Last-Modified")),
        allowDotfiles(conf.getAllowDotfiles()) {}
  DiskDirectoryService(config::DiskDirectory::Reader conf,
      kj::Own<const kj::ReadableDirectory> dir,
      kj::HttpHeaderTable::Builder& headerTableBuilder)
      : readable(kj::mv(dir)),
        headerTable(headerTableBuilder.getFutureTable()),
        hLastModified(headerTableBuilder.add("Last-Modified")),
        allowDotfiles(conf.getAllowDotfiles()) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return {this, kj::NullDisposer::instance};
  }

  kj::Maybe<const kj::Directory&> getWritable() {
    return writable;
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    return handlerName == "fetch"_kj;
  }

 private:
  kj::Maybe<const kj::Directory&> writable;
  kj::Own<const kj::ReadableDirectory> readable;
  kj::HttpHeaderTable& headerTable;
  kj::HttpHeaderId hLastModified;
  bool allowDotfiles;

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr urlStr,
      const kj::HttpHeaders& requestHeaders,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    TRACE_EVENT("workerd", "DiskDirectoryService::request()", "url", urlStr.cStr());
    auto url = kj::Url::parse(urlStr);

    bool blockedPath = false;
    kj::Path path = nullptr;
    KJ_IF_SOME(exception,
        kj::runCatchingExceptions([&]() { path = kj::Path(url.path.releaseAsArray()); })) {
      (void)exception;  // squash compiler warning about unused var
      // If the Path constructor throws, this path is not valid (e.g. it contains "..").
      blockedPath = true;
    }

    if (!blockedPath && !allowDotfiles) {
      for (auto& part: path) {
        if (part.startsWith(".")) {
          blockedPath = true;
          break;
        }
      }
    }

    if (method == kj::HttpMethod::GET || method == kj::HttpMethod::HEAD) {
      if (blockedPath) {
        co_return co_await response.sendError(404, "Not Found", headerTable);
      }

      auto file = KJ_UNWRAP_OR(readable->tryOpenFile(path),
          { co_return co_await response.sendError(404, "Not Found", headerTable); });

      auto meta = file->stat();

      switch (meta.type) {
        case kj::FsNode::Type::FILE: {
          // If this is a GET request with a Range header, return partial content if a single
          // satisfiable range is specified.
          // TODO(someday): consider supporting multiple ranges with multipart/byteranges
          kj::Maybe<kj::HttpByteRange> range;
          if (method == kj::HttpMethod::GET) {
            KJ_IF_SOME(header, requestHeaders.get(kj::HttpHeaderId::RANGE)) {
              KJ_SWITCH_ONEOF(kj::tryParseHttpRangeHeader(header.asArray(), meta.size)) {
                KJ_CASE_ONEOF(ranges, kj::Array<kj::HttpByteRange>) {
                  KJ_ASSERT(ranges.size() > 0);
                  if (ranges.size() == 1) range = ranges[0];
                }
                KJ_CASE_ONEOF(_, kj::HttpEverythingRange) {}
                KJ_CASE_ONEOF(_, kj::HttpUnsatisfiableRange) {
                  kj::HttpHeaders headers(headerTable);
                  headers.set(kj::HttpHeaderId::CONTENT_RANGE, kj::str("bytes */", meta.size));
                  co_return co_await response.sendError(416, "Range Not Satisfiable", headers);
                }
              }
            }
          }

          kj::HttpHeaders headers(headerTable);
          headers.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::OCTET_STREAM.toString());
          headers.set(hLastModified, httpTime(meta.lastModified));

          // We explicitly set the Content-Length header because if we don't, and we were called
          // by a local Worker (without an actual HTTP connection in between), then the Worker
          // will not see a Content-Length header, but being able to query the content length
          // (especially with HEAD requests) is quite useful.
          // TODO(cleanup): Arguably the implementation of `fetch()` should be adjusted so that
          //   if no `Content-Length` header is returned, but the body size is known via the KJ
          //   HTTP API, then the header should be filled in automatically. Unclear if this is safe
          //   to change without a compat flag.

          if (method == kj::HttpMethod::HEAD) {
            headers.set(kj::HttpHeaderId::CONTENT_LENGTH, kj::str(meta.size));
            response.send(200, "OK", headers, meta.size);
            co_return;
          } else KJ_IF_SOME(r, range) {
            KJ_ASSERT(r.start <= r.end);
            auto rangeSize = r.end - r.start + 1;
            headers.set(kj::HttpHeaderId::CONTENT_LENGTH, kj::str(rangeSize));
            headers.set(kj::HttpHeaderId::CONTENT_RANGE,
                kj::str("bytes ", r.start, "-", r.end, "/", meta.size));
            auto out = response.send(206, "Partial Content", headers, rangeSize);

            auto in = kj::heap<kj::FileInputStream>(*file, r.start);
            co_return co_await in->pumpTo(*out, rangeSize).ignoreResult();
          } else {
            headers.set(kj::HttpHeaderId::CONTENT_LENGTH, kj::str(meta.size));
            auto out = response.send(200, "OK", headers, meta.size);

            auto in = kj::heap<kj::FileInputStream>(*file);
            co_return co_await in->pumpTo(*out, meta.size).ignoreResult();
          }
        }
        case kj::FsNode::Type::DIRECTORY: {
          // Whoooops, we opened a directory. Back up and start over.

          auto dir = readable->openSubdir(path);

          kj::HttpHeaders headers(headerTable);
          headers.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());
          headers.set(hLastModified, httpTime(meta.lastModified));

          // We intentionally don't provide the expected size here in order to reserve the right
          // to switch to streaming directory listing in the future.
          auto out = response.send(200, "OK", headers);

          if (method == kj::HttpMethod::HEAD) {
            co_return;
          } else {
            auto entries = dir->listEntries();
            kj::Vector<kj::String> jsonEntries(entries.size());
            for (auto& entry: entries) {
              if (!allowDotfiles && entry.name.startsWith(".")) {
                continue;
              }

              kj::StringPtr type = "other";
              switch (entry.type) {
                case kj::FsNode::Type::FILE:
                  type = "file";
                  break;
                case kj::FsNode::Type::DIRECTORY:
                  type = "directory";
                  break;
                case kj::FsNode::Type::SYMLINK:
                  type = "symlink";
                  break;
                case kj::FsNode::Type::BLOCK_DEVICE:
                  type = "blockDevice";
                  break;
                case kj::FsNode::Type::CHARACTER_DEVICE:
                  type = "characterDevice";
                  break;
                case kj::FsNode::Type::NAMED_PIPE:
                  type = "namedPipe";
                  break;
                case kj::FsNode::Type::SOCKET:
                  type = "socket";
                  break;
                case kj::FsNode::Type::OTHER:
                  type = "other";
                  break;
              }

              jsonEntries.add(
                  kj::str("{\"name\":", escapeJsonString(entry.name), ",\"type\":\"", type, "\"}"));
            };

            auto content = kj::str('[', kj::strArray(jsonEntries, ","), ']');

            co_return co_await out->write(content.asBytes());
          }
        }
        default:
          co_return co_await response.sendError(406, "Not Acceptable", headerTable);
      }
    } else if (method == kj::HttpMethod::PUT) {
      auto& w = KJ_UNWRAP_OR(writable,
          { co_return co_await response.sendError(405, "Method Not Allowed", headerTable); });

      if (blockedPath || path.size() == 0) {
        co_return co_await response.sendError(403, "Unauthorized", headerTable);
      }

      auto replacer = w.replaceFile(
          path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
      auto stream = kj::heap<kj::FileOutputStream>(replacer->get());

      co_await requestBody.pumpTo(*stream);

      replacer->commit();
      kj::HttpHeaders headers(headerTable);
      response.send(204, "No Content", headers);
      co_return;
    } else if (method == kj::HttpMethod::DELETE) {
      auto& w = KJ_UNWRAP_OR(writable,
          { co_return co_await response.sendError(405, "Method Not Allowed", headerTable); });

      if (blockedPath || path.size() == 0) {
        co_return co_await response.sendError(403, "Unauthorized", headerTable);
      }

      auto found = w.tryRemove(path);

      kj::HttpHeaders headers(headerTable);
      if (found) {
        response.send(204, "No Content", headers);
        co_return;
      } else {
        co_return co_await response.sendError(404, "Not Found", headers);
      }
    } else {
      co_return co_await response.sendError(501, "Not Implemented", headerTable);
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      kj::HttpService::ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    throwUnsupported();
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    return kj::READY_NOW;
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    throwUnsupported();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    throwUnsupported();
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

  [[noreturn]] void throwUnsupported() {
    JSG_FAIL_REQUIRE(Error, "Disk directory services don't support this event type.");
  }
};

kj::Own<Server::Service> Server::makeDiskDirectoryService(kj::StringPtr name,
    config::DiskDirectory::Reader conf,
    kj::HttpHeaderTable::Builder& headerTableBuilder) {
  TRACE_EVENT("workerd", "Server::makeDiskDirectoryService()");
  kj::StringPtr pathStr = nullptr;
  kj::String ownPathStr;

  KJ_IF_SOME(override, directoryOverrides.findEntry(name)) {
    pathStr = ownPathStr = kj::mv(override.value);
    directoryOverrides.erase(override);
  } else if (conf.hasPath()) {
    pathStr = conf.getPath();
  } else {
    reportConfigError(kj::str("Directory \"", name,
        "\" has no path in the config, so must be specified on the "
        "command line with `--directory-path`."));
    return makeInvalidConfigService();
  }

  auto path = fs.getCurrentPath().evalNative(pathStr);

  if (conf.getWritable()) {
    auto openDir = KJ_UNWRAP_OR(fs.getRoot().tryOpenSubdir(kj::mv(path), kj::WriteMode::MODIFY), {
      reportConfigError(kj::str("Directory named \"", name, "\" not found: ", pathStr));
      return makeInvalidConfigService();
    });

    return kj::refcounted<DiskDirectoryService>(conf, kj::mv(openDir), headerTableBuilder);
  } else {
    auto openDir = KJ_UNWRAP_OR(fs.getRoot().tryOpenSubdir(kj::mv(path)), {
      reportConfigError(kj::str("Directory named \"", name, "\" not found: ", pathStr));
      return makeInvalidConfigService();
    });

    return kj::refcounted<DiskDirectoryService>(conf, kj::mv(openDir), headerTableBuilder);
  }
}

// =======================================================================================

// This class exists to update the InspectorService's table of isolates when a config
// has multiple services. The InspectorService exists on the stack of its own thread and
// initializes state that is bound to the thread, e.g. a http server and an event loop.
// This class provides a small thread-safe interface to the InspectorService so <name>:<isolate>
// mappings can be added after the InspectorService has started.
//
// The Cloudflare devtools only show the first service in workerd configuration. This service
// is always contains a users code. However, in packaging user code wrangler may add
// additional services that also have code. If using Chrome devtools to inspect a workerd,
// instance all services are visible and can be debugged.
class Server::InspectorServiceIsolateRegistrar final {
 public:
  InspectorServiceIsolateRegistrar() {}
  ~InspectorServiceIsolateRegistrar() noexcept(true);

  void registerIsolate(kj::StringPtr name, Worker::Isolate* isolate);

  KJ_DISALLOW_COPY_AND_MOVE(InspectorServiceIsolateRegistrar);

 private:
  void attach(const Server::InspectorService* anInspectorService) {
    *inspectorService.lockExclusive() = anInspectorService;
  }

  void detach() {
    *inspectorService.lockExclusive() = nullptr;
  }

  kj::MutexGuarded<const InspectorService*> inspectorService;
  friend class Server::InspectorService;
};

// Implements the interface for the devtools inspector protocol.
//
// The InspectorService is created when workerd serve is called using the -i option
// to define the inspector socket.
class Server::InspectorService final: public kj::HttpService, public kj::HttpServerErrorHandler {
 public:
  InspectorService(kj::Own<const kj::Executor> isolateThreadExecutor,
      kj::Timer& timer,
      kj::HttpHeaderTable::Builder& headerTableBuilder,
      InspectorServiceIsolateRegistrar& registrar)
      : isolateThreadExecutor(kj::mv(isolateThreadExecutor)),
        timer(timer),
        headerTable(headerTableBuilder.getFutureTable()),
        server(timer, headerTable, *this, kj::HttpServerSettings{.errorHandler = *this}),
        registrar(registrar) {
    registrar.attach(this);
  }

  ~InspectorService() {
    KJ_IF_SOME(r, registrar) {
      r.detach();
    }
  }

  void invalidateRegistrar() {
    registrar = kj::none;
  }

  kj::Promise<void> handleApplicationError(
      kj::Exception exception, kj::Maybe<kj::HttpService::Response&> response) override {
    if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
      // Don't send a response, just close connection.
      co_return;
    }
    KJ_LOG(ERROR, kj::str("Uncaught exception: ", exception));
    KJ_IF_SOME(r, response) {
      co_return co_await r.sendError(500, "Internal Server Error", headerTable);
    }
  }

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    // The inspector protocol starts with the debug client sending ordinary HTTP GET requests
    // to /json/version and then to /json or /json/list. These must respond with valid JSON
    // documents that list the details of what isolates are available for inspection. Each
    // isolate must be listed separately. In the advertisement for each isolate is a URL
    // and a unique ID. The client will use the URL and ID to open a WebSocket request to
    // actually connect the debug session.
    kj::HttpHeaders responseHeaders(headerTable);
    if (headers.isWebSocket()) {
      KJ_IF_SOME(pos, url.findLast('/')) {
        auto id = url.slice(pos + 1);

        KJ_IF_SOME(isolate, isolates.find(id)) {
          // If getting the strong ref doesn't work it means that the Worker::Isolate
          // has already been cleaned up. We use a weak ref here in order to keep from
          // having the Worker::Isolate itself having to know anything at all about the
          // IsolateService and the registration process. So instead of having Isolate
          // explicitly clean up after itself we lazily evaluate the weak ref and clean
          // up when necessary.
          KJ_IF_SOME(ref, isolate->tryAddStrongRef()) {
            // When using --verbose, we'll output some logging to indicate when the
            // inspector client is attached/detached.
            KJ_LOG(INFO, kj::str("Inspector client attaching [", id, "]"));
            auto webSocket = response.acceptWebSocket(responseHeaders);
            kj::Duration timerOffset = 0 * kj::MILLISECONDS;
            try {
              co_return co_await ref->attachInspector(
                  isolateThreadExecutor->addRef(), timer, timerOffset, *webSocket);
            } catch (...) {
              auto exception = kj::getCaughtExceptionAsKj();
              if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
                // This likely just means that the inspector client was closed.
                // Nothing to do here but move along.
                KJ_LOG(INFO, "Inspector client detached"_kj);
                co_return;
              } else {
                // If it's any other kind of error, propagate it!
                kj::throwFatalException(kj::mv(exception));
              }
            }
          } else {
            // If we can't get a strong ref to the isolate here, it's been cleaned
            // up. The only thing we're going to do is clean up here and act like
            // nothing happened.
            isolates.erase(id);
          }
        }

        KJ_LOG(INFO, kj::str("Unknown worker session [", id, "]"));
        co_return co_await response.sendError(404, "Unknown worker session", responseHeaders);
      }

      // No / in url!? That's weird
      co_return co_await response.sendError(400, "Invalid request", responseHeaders);
    }

    // If the request is not a WebSocket request, it must be a GET to fetch details
    // about the implementation.
    if (method != kj::HttpMethod::GET) {
      co_return co_await response.sendError(501, "Unsupported Operation", responseHeaders);
    }

    if (url.endsWith("/json/version")) {
      responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());
      auto content = kj::str("{\"Browser\": \"workerd\", \"Protocol-Version\": \"1.3\" }");
      auto out = response.send(200, "OK", responseHeaders, content.size());
      co_return co_await out->write(content.asBytes());
    } else if (url.endsWith("/json") || url.endsWith("/json/list") ||
        url.endsWith("/json/list?for_tab")) {
      responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());

      auto baseWsUrl = KJ_UNWRAP_OR(headers.get(kj::HttpHeaderId::HOST),
          { co_return co_await response.sendError(400, "Bad Request", responseHeaders); });

      kj::Vector<kj::String> entries(isolates.size());
      kj::Vector<kj::String> toRemove;
      for (auto& entry: isolates) {
        // While we don't actually use the strong ref here we still attempt to acquire it
        // in order to determine if the isolate is actually still around. If the isolate
        // has been destroyed the weak ref will be cleared. We do it this way to keep from
        // having the Worker::Isolate know anything at all about the InspectorService.
        // We'll lazily clean up whenever we detect that the ref has been invalidated.
        //
        // TODO(cleanup): If we ever enable reloading of isolates for live services, we may
        // want to refactor this such that the WorkerService holds a handle to the registration
        // as opposed to using this lazy cleanup mechanism. For now, however, this is
        // sufficient.
        KJ_IF_SOME(ref, entry.value->tryAddStrongRef()) {
          (void)ref;  // squash compiler warning about unused ref
          kj::Vector<kj::String> fields(9);
          fields.add(kj::str("\"id\":\"", entry.key, "\""));
          fields.add(kj::str("\"title\":\"workerd: worker ", entry.key, "\""));
          fields.add(kj::str("\"type\":\"node\""));
          fields.add(kj::str("\"description\":\"workerd worker\""));
          fields.add(kj::str("\"webSocketDebuggerUrl\":\"ws://", baseWsUrl, "/", entry.key, "\""));
          fields.add(kj::str(
              "\"devtoolsFrontendUrl\":\"devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=",
              baseWsUrl, "/\""));
          fields.add(kj::str(
              "\"devtoolsFrontendUrlCompat\":\"devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=",
              baseWsUrl, "/\""));
          fields.add(kj::str("\"faviconUrl\":\"https://workers.cloudflare.com/favicon.ico\""));
          fields.add(kj::str("\"url\":\"https://workers.dev\""));
          entries.add(kj::str('{', kj::strArray(fields, ","), '}'));
        } else {
          // If we're not able to get a reference to the isolate here, it's
          // been cleaned up and we should remove it from the list. We do this
          // after iterating to make sure we don't invalidate the iterator.
          toRemove.add(kj::str(entry.key));
        }
      }
      // Clean up if necessary
      for (auto& key: toRemove) {
        isolates.erase(key);
      }

      auto content = kj::str('[', kj::strArray(entries, ","), ']');

      auto out = response.send(200, "OK", responseHeaders, content.size());
      co_return co_await out->write(content.asBytes()).attach(kj::mv(content), kj::mv(out));
    }

    co_return co_await response.sendError(500, "Not yet implemented", responseHeaders);
  }

  kj::Promise<void> listen(kj::Own<kj::ConnectionReceiver> listener) {
    // Note that we intentionally do not make inspector connections be part of the usual drain()
    // procedure. Inspector connections are always long-lived WebSockets, and we do not want the
    // existence of such a connection to hold the server open. We do, however, want the connection
    // to stay open until all other requests are drained, for debugging purposes.
    //
    // Thus:
    // * We let connection loop tasks live on `HttpServer`'s own `TaskSet`, rather than our
    //   server's main `TaskSet` which we wait to become empty on drain.
    // * We do not add this `HttpServer` to the server's `httpServers` list, so it will not receive
    //   drain() requests. (However, our caller does cancel listening on the server port as soon
    //   as we begin draining, since we may want new connections to go to a new instance of the
    //   server.)
    co_return co_await server.listenHttp(*listener);
  }

  void registerIsolate(kj::StringPtr name, Worker::Isolate* isolate) {
    isolates.insert(kj::str(name), isolate->getWeakRef());
  }

 private:
  kj::Own<const kj::Executor> isolateThreadExecutor;
  kj::Timer& timer;
  kj::HttpHeaderTable& headerTable;
  kj::HashMap<kj::String, kj::Own<const Worker::Isolate::WeakIsolateRef>> isolates;
  kj::HttpServer server;
  kj::Maybe<InspectorServiceIsolateRegistrar&> registrar;
};

Server::InspectorServiceIsolateRegistrar::~InspectorServiceIsolateRegistrar() noexcept(true) {
  auto lockedInspectorService = this->inspectorService.lockExclusive();
  if (lockedInspectorService != nullptr) {
    auto is = const_cast<InspectorService*>(*lockedInspectorService);
    is->invalidateRegistrar();
  }
}

void Server::InspectorServiceIsolateRegistrar::registerIsolate(
    kj::StringPtr name, Worker::Isolate* isolate) {
  auto lockedInspectorService = this->inspectorService.lockExclusive();
  if (lockedInspectorService != nullptr) {
    auto is = const_cast<InspectorService*>(*lockedInspectorService);
    is->registerIsolate(name, isolate);
  }
}

// =======================================================================================
namespace {
class RequestObserverWithTracer final: public RequestObserver, public WorkerInterface {
 public:
  RequestObserverWithTracer(kj::Maybe<kj::Own<WorkerTracer>> tracer, kj::TaskSet& waitUntilTasks)
      : tracer(kj::mv(tracer)) {}

  ~RequestObserverWithTracer() noexcept(false) {
    KJ_IF_SOME(t, tracer) {
      // for a more precise end time, set the end timestamp now, if available
      KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
        auto time = ioContext.now();
        t->recordTimestamp(time);
      }
      t->setOutcome(
          outcome, 0 * kj::MILLISECONDS /* cpu time */, 0 * kj::MILLISECONDS /* wall time */);
    }
  }

  WorkerInterface& wrapWorkerInterface(WorkerInterface& worker) override {
    if (tracer != kj::none) {
      inner = worker;
      return *this;
    }
    return worker;
  }

  void reportFailure(const kj::Exception& exception, FailureSource source) override {
    outcome = EventOutcome::EXCEPTION;
  }

  void setOutcome(EventOutcome newOutcome) override {
    outcome = newOutcome;
  }

  // WorkerInterface
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    try {
      SimpleResponseObserver responseWrapper(&fetchStatus, response);
      co_await KJ_ASSERT_NONNULL(inner).request(method, url, headers, requestBody, responseWrapper);
    } catch (...) {
      fetchStatus = 500;
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception, FailureSource::OTHER);
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    try {
      co_return co_await KJ_ASSERT_NONNULL(inner).connect(
          host, headers, connection, response, settings);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception, FailureSource::OTHER);
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    try {
      co_return co_await KJ_ASSERT_NONNULL(inner).prewarm(url);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception, FailureSource::OTHER);
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    try {
      co_return co_await KJ_ASSERT_NONNULL(inner).runScheduled(scheduledTime, cron);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception, FailureSource::OTHER);
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    try {
      co_return co_await KJ_ASSERT_NONNULL(inner).runAlarm(scheduledTime, retryCount);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception, FailureSource::OTHER);
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Promise<bool> test() override {
    try {
      co_return co_await KJ_ASSERT_NONNULL(inner).test();
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception, FailureSource::OTHER);
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    try {
      co_return co_await KJ_ASSERT_NONNULL(inner).customEvent(kj::mv(event));
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception, FailureSource::OTHER);
      kj::throwFatalException(kj::mv(exception));
    }
  }

 private:
  kj::Maybe<kj::Own<WorkerTracer>> tracer;
  kj::Maybe<WorkerInterface&> inner;
  EventOutcome outcome = EventOutcome::OK;
  kj::uint fetchStatus = 0;
};

class SequentialSpanSubmitter final: public SpanSubmitter {
 public:
  SequentialSpanSubmitter(kj::Own<WorkerTracer> workerTracer): workerTracer(kj::mv(workerTracer)) {}
  void submitSpan(tracing::SpanId spanId, tracing::SpanId parentSpanId, const Span& span) override {
    // We largely recreate the span here which feels inefficient, but is hard to avoid given the
    // mismatch between the Span type and the full span information required for OTel.
    tracing::CompleteSpan span2(
        spanId, parentSpanId, span.operationName.clone(), span.startTime, span.endTime);
    span2.tags.reserve(span.tags.size());
    for (auto& tag: span.tags) {
      span2.tags.insert(tag.key.clone(), spanTagClone(tag.value));
    }
    if (isPredictableModeForTest()) {
      span2.startTime = span2.endTime = kj::UNIX_EPOCH;
    }

    workerTracer->addSpan(kj::mv(span2));
  }

  tracing::SpanId makeSpanId() override {
    return tracing::SpanId(nextSpanId++);
  }
  KJ_DISALLOW_COPY_AND_MOVE(SequentialSpanSubmitter);

 private:
  uint64_t nextSpanId = 1;
  kj::Own<WorkerTracer> workerTracer;
};

// IsolateLimitEnforcer that enforces no limits.
class NullIsolateLimitEnforcer final: public IsolateLimitEnforcer {
 public:
  v8::Isolate::CreateParams getCreateParams() override {
    return {};
  }

  void customizeIsolate(v8::Isolate* isolate) override {}

  ActorCacheSharedLruOptions getActorCacheLruOptions() override {
    // TODO(someday): Make this configurable?
    return {.softLimit = 16 * (1ull << 20),  // 16 MiB
      .hardLimit = 128 * (1ull << 20),       // 128 MiB
      .staleTimeout = 30 * kj::SECONDS,
      .dirtyListByteLimit = 8 * (1ull << 20),  // 8 MiB
      .maxKeysPerRpc = 128,

      // For now, we use `neverFlush` to implement in-memory-only actors.
      // See WorkerService::getActor().
      .neverFlush = true};
  }

  kj::Own<void> enterStartupJs(
      jsg::Lock& lock, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }

  kj::Own<void> enterStartupPython(
      jsg::Lock& lock, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }

  kj::Own<void> enterDynamicImportJs(
      jsg::Lock& lock, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }

  kj::Own<void> enterLoggingJs(
      jsg::Lock& lock, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }

  kj::Own<void> enterInspectorJs(
      jsg::Lock& loc, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }

  void completedRequest(kj::StringPtr id) const override {}

  bool exitJs(jsg::Lock& lock) const override {
    return false;
  }

  void reportMetrics(IsolateObserver& isolateMetrics) const override {}

  kj::Maybe<size_t> checkPbkdfIterations(jsg::Lock& lock, size_t iterations) const override {
    // No limit on the number of iterations in workerd
    return kj::none;
  }

  bool hasExcessivelyExceededHeapLimit() const override {
    return false;
  }
};

}  // namespace

// Shared ErrorReporter base implemnetation. The logic to collect entrypoint information is the
// same regardless of where the code came from.
struct Server::ErrorReporter: public Worker::ValidationErrorReporter {
  // The `HashSet`s are the set of exported handlers, like `fetch`, `test`, etc.
  kj::HashMap<kj::String, kj::HashSet<kj::String>> namedEntrypoints;
  kj::Maybe<kj::HashSet<kj::String>> defaultEntrypoint;
  kj::HashSet<kj::String> actorClasses;
  kj::HashSet<kj::String> workflowClasses;

  void addEntrypoint(kj::Maybe<kj::StringPtr> exportName, kj::Array<kj::String> methods) override {
    kj::HashSet<kj::String> set;
    for (auto& method: methods) {
      set.insert(kj::mv(method));
    }
    KJ_IF_SOME(e, exportName) {
      namedEntrypoints.insert(kj::str(e), kj::mv(set));
    } else {
      defaultEntrypoint = kj::mv(set);
    }
  }

  void addActorClass(kj::StringPtr exportName) override {
    actorClasses.insert(kj::str(exportName));
  }

  void addWorkflowClass(kj::StringPtr exportName, kj::Array<kj::String> methods) override {
    // At runtime, we need to add it into the normal namedEntrypoints for Workflows to appear
    // in `WorkerService`. This is a different method compared to `addEntrypoint` because we need to
    // check for `WorkflowEntrypoint` inheritance at validation time.
    kj::HashSet<kj::String> set;
    for (auto& method: methods) {
      set.insert(kj::mv(method));
    }
    namedEntrypoints.insert(kj::str(exportName), kj::mv(set));
    workflowClasses.insert(kj::str(exportName));
  }
};

// Implementation of ErrorReporter specifically for reporting errors in the top-level workerd
// config.
struct Server::ConfigErrorReporter final: public ErrorReporter {
  ConfigErrorReporter(Server& server, kj::StringPtr name): server(server), name(name) {}

  Server& server;
  kj::StringPtr name;

  void addError(kj::String error) override {
    server.handleReportConfigError(kj::str("service ", name, ": ", error));
  }
};

// Implementation of ErrorReporter for dynamically-loaded Workers. We'll collect the errors and
// report them in an exception at the end.
struct Server::DynamicErrorReporter final: public ErrorReporter {
  kj::Vector<kj::String> errors;

  void addError(kj::String error) override {
    errors.add(kj::mv(error));
  }

  void throwIfErrors() {
    if (!errors.empty()) {
      JSG_FAIL_REQUIRE(Error, "Failed to start Worker:\n", kj::strArray(errors, "\n"));
    }
  }
};

class Server::WorkerService final: public Service,
                                   private kj::TaskSet::ErrorHandler,
                                   private IoChannelFactory,
                                   private TimerChannel,
                                   private LimitEnforcer {
 public:
  class ActorNamespace;

  // I/O channels, delivered when link() is called.
  struct LinkedIoChannels {
    kj::Array<kj::Own<IoChannelFactory::SubrequestChannel>> subrequest;
    kj::Array<kj::Maybe<ActorNamespace&>> actor;  // null = configuration error
    kj::Array<kj::Own<ActorClass>> actorClass;
    kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> cache;
    kj::Maybe<const kj::Directory&> actorStorage;
    AlarmScheduler& alarmScheduler;
    kj::Array<kj::Own<IoChannelFactory::SubrequestChannel>> tails;
    kj::Array<kj::Own<IoChannelFactory::SubrequestChannel>> streamingTails;
    kj::Array<kj::Rc<WorkerLoaderNamespace>> workerLoaders;
    kj::Maybe<kj::Network&> workerdDebugPortNetwork;
  };
  using LinkCallback =
      kj::Function<LinkedIoChannels(WorkerService&, Worker::ValidationErrorReporter&)>;
  using AbortActorsCallback = kj::Function<void(kj::Maybe<const kj::Exception&> reason)>;

  WorkerService(ChannelTokenHandler& channelTokenHandler,
      kj::Maybe<kj::StringPtr> serviceName,
      ThreadContext& threadContext,
      const kj::MonotonicClock& monotonicClock,
      kj::Own<const Worker> worker,
      kj::Maybe<kj::HashSet<kj::String>> defaultEntrypointHandlers,
      kj::HashMap<kj::String, kj::HashSet<kj::String>> namedEntrypoints,
      kj::HashSet<kj::String> actorClassEntrypointsParam,
      LinkCallback linkCallback,
      AbortActorsCallback abortActorsCallback,
      kj::Maybe<kj::String> dockerPathParam,
      kj::Maybe<kj::String> containerEgressInterceptorImageParam,
      bool isDynamic)
      : channelTokenHandler(channelTokenHandler),
        serviceName(serviceName),
        threadContext(threadContext),
        monotonicClock(monotonicClock),
        ioChannels(kj::mv(linkCallback)),
        worker(kj::mv(worker)),
        defaultEntrypointHandlers(kj::mv(defaultEntrypointHandlers)),
        namedEntrypoints(kj::mv(namedEntrypoints)),
        actorClassEntrypoints(kj::mv(actorClassEntrypointsParam)),
        waitUntilTasks(*this),
        abortActorsCallback(kj::mv(abortActorsCallback)),
        dockerPath(kj::mv(dockerPathParam)),
        containerEgressInterceptorImage(kj::mv(containerEgressInterceptorImageParam)),
        isDynamic(isDynamic) {}

  // Call immediately after the constructor to set up `actorNamespaces`. This can't happen during
  // the constructor itself since it sets up cyclic references, which will throw an exception if
  // done during the constructor.
  void initActorNamespaces(
      const kj::HashMap<kj::String, ActorConfig>& actorClasses, kj::Network& network) {
    actorNamespaces.reserve(actorClasses.size());
    for (auto& entry: actorClasses) {
      if (!actorClassEntrypoints.contains(entry.key)) {
        KJ_LOG(WARNING,
            kj::str("A DurableObjectNamespace in the config referenced the class \"", entry.key,
                "\", but no such Durable Object class is exported from the worker. Please make "
                "sure the class name matches, it is exported, and the class extends "
                "'DurableObject'. Attempts to call to this Durable Object class will fail at "
                "runtime, but historically this was not a startup-time error. Future versions of "
                "workerd may make this a startup-time error."));
      }

      auto actorClass = kj::refcounted<ActorClassImpl>(*this, entry.key, Frankenvalue());
      auto ns = kj::heap<ActorNamespace>(kj::mv(actorClass), entry.value,
          threadContext.getUnsafeTimer(), threadContext.getByteStreamFactory(), channelTokenHandler,
          network, dockerPath, containerEgressInterceptorImage, waitUntilTasks);
      actorNamespaces.insert(entry.key, kj::mv(ns));
    }
  }

  void requireAllowsTransfer() override {
    if (isDynamic) throwDynamicEntrypointTransferError();
  }

  kj::Maybe<kj::Own<Service>> getEntrypoint(kj::Maybe<kj::StringPtr> name, Frankenvalue props) {
    const kj::HashSet<kj::String>* handlers;
    KJ_IF_SOME(n, name) {
      KJ_IF_SOME(entry, namedEntrypoints.findEntry(n)) {
        name = entry.key;  // replace with more-permanent string
        handlers = &entry.value;
      } else KJ_IF_SOME(className, actorClassEntrypoints.find(n)) {
        // TODO(soon): Restore this warning once miniflare no longer generates config that causes
        //   it to log spuriously.
        //
        // KJ_LOG(WARNING,
        //     kj::str("A ServiceDesignator in the config referenced the entrypoint \"", n,
        //         "\", but this class does not extend 'WorkerEntrypoint'. Attempts to call this "
        //         "entrypoint will fail at runtime, but historically this was not a startup-time "
        //         "error. Future versions of workerd may make this a startup-time error."));

        static const kj::HashSet<kj::String> EMPTY_HANDLERS;
        name = className;  // replace with more-permanent string
        handlers = &EMPTY_HANDLERS;
      } else {
        return kj::none;
      }
    } else {
      KJ_IF_SOME(d, defaultEntrypointHandlers) {
        handlers = &d;
      } else {
        // It would appear that there is no default export, therefore this refers to an entrypoint
        // that doesn't exist! However, this was historically allowed. For backwards-compatibility,
        // we preserve this behavior, by returning a reference to the WorkerService itself, whose
        // startRequest() will fail.
        //
        // What will happen if you invoke this entrypoint? Not what you think. Check out the
        // test case in server-test.c++ entitled "referencing non-extant default entrypoint is not
        // an error" for the sordid details.
        return kj::addRef(*this);
      }
    }
    return kj::refcounted<EntrypointService>(*this, name, kj::mv(props), *handlers);
  }

  // Like getEntrypoint() but used specifically to get the entrypoint for use in ctx.exports,
  // where it can be used raw (props are empty), or can be specialized with props.
  kj::Own<Service> getLoopbackEntrypoint(kj::Maybe<kj::StringPtr> name) {
    const kj::HashSet<kj::String>* handlers;
    KJ_IF_SOME(n, name) {
      KJ_IF_SOME(entry, namedEntrypoints.findEntry(n)) {
        name = entry.key;  // replace with more-permanent string
        handlers = &entry.value;
      } else {
        KJ_FAIL_REQUIRE("getLoopbackEntrypoint() called for entrypoint that doesn't exist");
      }
    } else {
      KJ_IF_SOME(d, defaultEntrypointHandlers) {
        handlers = &d;
      } else {
        KJ_FAIL_REQUIRE("getLoopbackEntrypoint() called for entrypoint that doesn't exist");
      }
    }
    return kj::refcounted<EntrypointService>(*this, name, kj::none, *handlers);
  }

  kj::Maybe<kj::Own<ActorClass>> getActorClass(kj::Maybe<kj::StringPtr> name, Frankenvalue props) {
    KJ_IF_SOME(className, actorClassEntrypoints.find(KJ_UNWRAP_OR(name, return kj::none))) {
      return kj::refcounted<ActorClassImpl>(*this, className, kj::mv(props));
    } else {
      return kj::none;
    }
  }

  kj::Own<ActorClass> getLoopbackActorClass(kj::StringPtr name) {
    // Look up a more permanent class name string. (Also validates this is actually an export.)
    kj::StringPtr className = KJ_REQUIRE_NONNULL(actorClassEntrypoints.find(name),
        "getLoopbackActorClass() called for actor class that doesn't exist");

    return kj::refcounted<ActorClassImpl>(*this, className, kj::none);
  }

  bool hasDefaultEntrypoint() {
    return defaultEntrypointHandlers != kj::none;
  }

  kj::Array<kj::StringPtr> getEntrypointNames() {
    return KJ_MAP(e, namedEntrypoints) -> kj::StringPtr { return e.key; };
  }

  kj::Array<kj::StringPtr> getActorClassNames() {
    return KJ_MAP(name, actorClassEntrypoints) -> kj::StringPtr { return name; };
  }

  void link(Worker::ValidationErrorReporter& errorReporter) override {
    LinkCallback callback =
        kj::mv(KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkCallback>(), "already called link()"));
    auto linked = callback(*this, errorReporter);

    for (auto& ns: actorNamespaces) {
      ns.value->link(linked.actorStorage, linked.alarmScheduler);
    }

    ioChannels = kj::mv(linked);
  }

  void unlink() override {
    // Need to remove all waited until tasks before destroying `ioChannels`
    waitUntilTasks.clear();

    // Need to tear down all actors before tearing down `ioChannels.actorStorage`.
    actorNamespaces.clear();

    // OK, now we can unlink.
    ioChannels = {};
  }

  kj::Maybe<ActorNamespace&> getActorNamespace(kj::StringPtr name) {
    KJ_IF_SOME(a, actorNamespaces.find(name)) {
      return *a;
    } else {
      return kj::none;
    }
  }

  kj::HashMap<kj::StringPtr, kj::Own<ActorNamespace>>& getActorNamespaces() {
    return actorNamespaces;
  }

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return startRequest(kj::mv(metadata), kj::none, {});
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    KJ_IF_SOME(h, defaultEntrypointHandlers) {
      return h.contains(handlerName);
    } else {
      return false;
    }
  }

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata,
      kj::Maybe<kj::StringPtr> entrypointName,
      Frankenvalue props,
      kj::Maybe<kj::Own<Worker::Actor>> actor = kj::none,
      bool isTracer = false) {
    TRACE_EVENT("workerd", "Server::WorkerService::startRequest()");

    auto& channels = KJ_ASSERT_NONNULL(ioChannels.tryGet<LinkedIoChannels>());

    kj::Vector<kj::Own<WorkerInterface>> bufferedTailWorkers(channels.tails.size());
    kj::Vector<kj::Own<WorkerInterface>> streamingTailWorkers(channels.streamingTails.size());
    auto addWorkerIfNotRecursiveTracer = [this, isTracer](
                                             kj::Vector<kj::Own<WorkerInterface>>& workers,
                                             IoChannelFactory::SubrequestChannel& channel) {
      // Caution here... if the tail worker ends up having a circular dependency
      // on the worker we'll end up with an infinite loop trying to initialize.
      // We can test this directly but it's more difficult to test indirect
      // loops (dependency of dependency, etc). Here we're just going to keep
      // it simple and just check the direct dependency.
      // If service refers to an EntrypointService, we need to compare with the underlying
      // WorkerService to match this.
      auto& service = KJ_UNWRAP_OR(kj::dynamicDowncastIfAvailable<Service>(channel), {
        // Not a Service, probably not self-referential.
        workers.add(channel.startRequest({}));
        return;
      });

      if (service.service() == this) {
        if (!isTracer) {
          // This is a self-reference. Create a request with isTracer=true.
          KJ_IF_SOME(s, kj::dynamicDowncastIfAvailable<WorkerService>(service)) {
            workers.add(s.startRequest({}, kj::none, {}, kj::none, true));
          } else KJ_IF_SOME(s, kj::dynamicDowncastIfAvailable<EntrypointService>(service)) {
            workers.add(s.startRequest({}, true));
          } else {
            KJ_FAIL_ASSERT("Unexpected service type in recursive tail worker declaration");
          }
        } else {
          // Intentionally left empty to prevent infinite recursion with tail workers tailing
          // themselves
        }
      } else {
        workers.add(service.startRequest({}));
      }
    };

    // Do not add tracers for worker interfaces with the "test" entrypoint – we generally do not
    // need to trace the test event, although this is useful to test that span tracing works, so
    // we are not implementing a (more complex) mechanism to disable tracing for all test() events
    // here.
    if (entrypointName.orDefault("") != "test"_kj) {
      for (auto& service: channels.tails) {
        addWorkerIfNotRecursiveTracer(bufferedTailWorkers, *service);
      }
      for (auto& service: channels.streamingTails) {
        addWorkerIfNotRecursiveTracer(streamingTailWorkers, *service);
      }
    }

    kj::Maybe<kj::Own<WorkerTracer>> workerTracer = kj::none;

    if (!bufferedTailWorkers.empty() || !streamingTailWorkers.empty()) {
      // Setting up buffered tail workers support, but only if we actually have tail workers
      // configured.
      auto executionModel =
          actor == kj::none ? ExecutionModel::STATELESS : ExecutionModel::DURABLE_OBJECT;
      auto tailStreamWriter = tracing::initializeTailStreamWriter(
          streamingTailWorkers.releaseAsArray(), waitUntilTasks);
      auto trace = kj::refcounted<Trace>(kj::none /* stableId */, kj::none /* scriptName */,
          kj::none /* scriptVersion */, kj::none /* dispatchNamespace */, kj::none /* scriptId */,
          nullptr /* scriptTags */, mapCopyString(entrypointName), executionModel,
          kj::none /* durableObjectId */);
      kj::Own<WorkerTracer> tracer = kj::refcounted<WorkerTracer>(
          kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::mv(tailStreamWriter));

      // When the tracer is complete, deliver traces to any buffered tail workers. We end up
      // creating two references to the WorkerTracer, one held by the observer and one that will be
      // passed to the IoContext. This ensures that the tracer lives long enough to receive all
      // events.
      if (!bufferedTailWorkers.empty()) {
        waitUntilTasks.add(tracer->onComplete().then(
            kj::coCapture([tailWorkers = bufferedTailWorkers.releaseAsArray()](
                              kj::Own<Trace> trace) mutable -> kj::Promise<void> {
          for (auto& worker: tailWorkers) {
            auto event = kj::heap<workerd::api::TraceCustomEvent>(
                workerd::api::TraceCustomEvent::TYPE, kj::arr(kj::addRef(*trace)));
            co_await worker->customEvent(kj::mv(event)).ignoreResult();
          }
          co_return;
        })));
      }
      workerTracer = kj::mv(tracer);
    }

    KJ_IF_SOME(w, workerTracer) {
      w->setMakeUserRequestSpanFunc([&w = *w]() {
        return SpanParent(kj::refcounted<UserSpanObserver>(
            kj::refcounted<SequentialSpanSubmitter>(kj::addRef(w))));
      });
    }
    kj::Own<RequestObserver> observer =
        kj::refcounted<RequestObserverWithTracer>(mapAddRef(workerTracer), waitUntilTasks);

    return newWorkerEntrypoint(threadContext, kj::atomicAddRef(*worker), entrypointName,
        kj::mv(props), kj::mv(actor), kj::Own<LimitEnforcer>(this, kj::NullDisposer::instance),
        {},  // ioContextDependency
        kj::Own<IoChannelFactory>(this, kj::NullDisposer::instance), kj::mv(observer),
        waitUntilTasks,
        true,                  // tunnelExceptions
        kj::mv(workerTracer),  // workerTracer
        kj::mv(metadata.cfBlobJson));
  }

  class ActorNamespace final {
   public:
    ActorNamespace(kj::Own<ActorClass> actorClass,
        const ActorConfig& config,
        kj::Timer& timer,
        capnp::ByteStreamFactory& byteStreamFactory,
        ChannelTokenHandler& channelTokenHandler,
        kj::Network& dockerNetwork,
        kj::Maybe<kj::StringPtr> dockerPath,
        kj::Maybe<kj::StringPtr> containerEgressInterceptorImage,
        kj::TaskSet& waitUntilTasks)
        : actorClass(kj::mv(actorClass)),
          config(config),
          timer(timer),
          byteStreamFactory(byteStreamFactory),
          channelTokenHandler(channelTokenHandler),
          dockerNetwork(dockerNetwork),
          dockerPath(dockerPath),
          containerEgressInterceptorImage(containerEgressInterceptorImage),
          waitUntilTasks(waitUntilTasks) {}

    // Called at link time to provide needed resources.
    void link(kj::Maybe<const kj::Directory&> serviceActorStorage,
        kj::Maybe<AlarmScheduler&> alarmScheduler) {
      KJ_IF_SOME(dir, serviceActorStorage) {
        KJ_IF_SOME(d, config.tryGet<Durable>()) {
          // Create a subdirectory for this namespace based on the unique key.
          this->actorStorage.emplace(dir.openSubdir(
              kj::Path({d.uniqueKey}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY));
        }
      }

      this->alarmScheduler = alarmScheduler;
    }

    const ActorConfig& getConfig() {
      return config;
    }

    kj::Own<IoChannelFactory::ActorChannel> getActorChannel(Worker::Actor::Id id) {
      KJ_IF_SOME(doId, id.tryGet<kj::Own<ActorIdFactory::ActorId>>()) {
        // To emulate production, we have to recreate this ID.
        ActorIdFactoryImpl::ActorIdImpl* idImpl =
            dynamic_cast<ActorIdFactoryImpl::ActorIdImpl*>(doId.get());
        KJ_ASSERT(idImpl != nullptr, "Unexpected ActorId type?");
        idImpl->clearName();
      }

      return kj::refcounted<ActorChannelImpl>(getActorContainer(kj::mv(id)));
    }

    class ActorContainer;
    using ActorMap = kj::HashMap<kj::StringPtr, kj::Own<ActorContainer>>;

    // ActorContainer mostly serves as a wrapper around Worker::Actor.
    // We use it to associate a HibernationManager with the Worker::Actor, since the
    // Worker::Actor can be destroyed during periods of prolonged inactivity.
    //
    // We use a RequestTracker to track strong references to this ActorContainer's Worker::Actor.
    // Once there are no Worker::Actor's left (excluding our own), `inactive()` is triggered and we
    // initiate the eviction of the Durable Object. If no requests arrive in the next 10 seconds,
    // the DO is evicted, otherwise we cancel the eviction task.
    class ActorContainer final: public RequestTracker::Hooks,
                                public kj::Refcounted,
                                public Worker::Actor::FacetManager {
     public:
      // Information which is needed before start() can be called, but may not be available yet
      // when the ActorContainer is constructed (especially in the case of facets).
      struct ClassAndId {
        kj::Own<ActorClass> actorClass;
        Worker::Actor::Id id;

        ClassAndId(kj::Own<ActorClass> actorClass, Worker::Actor::Id id)
            : actorClass(kj::mv(actorClass)),
              id(kj::mv(id)) {}
      };

      ActorContainer(kj::String key,
          ActorNamespace& ns,
          kj::Maybe<ActorContainer&> parent,
          kj::OneOf<ClassAndId, kj::Promise<ClassAndId>> classAndIdParam,
          kj::Timer& timer)
          : key(kj::mv(key)),
            tracker(kj::refcounted<RequestTracker>(*this)),
            ns(ns),
            root(parent.map([](ActorContainer& p) -> ActorContainer& { return p.root; })
                     .orDefault(*this)),
            parent(parent),
            timer(timer),
            lastAccess(timer.now()) {
        KJ_SWITCH_ONEOF(classAndIdParam) {
          KJ_CASE_ONEOF(value, ClassAndId) {
            // `classAndId` is immediately available.
            classAndId = kj::mv(value);
          }
          KJ_CASE_ONEOF(promise, kj::Promise<ClassAndId>) {
            // We are receiving a promise for a `ClassAndId` to come later. Arrange to initialize
            // `classAndId` from the promise. Create a `ForkedPromise<void>` that resolves when
            // initialization is complete.
            classAndId = promise
                             .then([this](ClassAndId value) {
              auto& forked = KJ_ASSERT_NONNULL(classAndId.tryGet<kj::ForkedPromise<void>>());
              if (!forked.hasBranches()) {
                // HACK: We're about to replace the ForkedPromise but it has no one waiting on it,
                //   so we'd end up cancelling ourselves. Add a branch and detach it so this doesn't
                //   happen.
                forked.addBranch().detach([](auto&&) {});
              }

              classAndId = kj::mv(value);
            }).fork();
          }
        }
      }

      ~ActorContainer() noexcept(false) {
        // Shutdown the tracker so we don't use active/inactive hooks anymore.
        tracker->shutdown();

        for (auto& facet: facets) {
          facet.value->abort(kj::none);
        }

        KJ_IF_SOME(a, actor) {
          // Unknown broken reason.
          auto reason = 0;
          a->shutdown(reason);
        }

        // Drop the container client reference
        // If setInactivityTimeout() was called, there's still a timer holding a reference
        // If not, this may be the last reference and the ContainerClient destructor will run
        containerClient = kj::none;
      }

      void active() override {
        // We're handling a new request, cancel the eviction promise.
        shutdownTask = kj::none;
      }

      void inactive() override {
        // Durable objects are evictable by default.
        bool isEvictable = true;
        KJ_SWITCH_ONEOF(ns.config) {
          KJ_CASE_ONEOF(c, Durable) {
            isEvictable = c.isEvictable;
          }
          KJ_CASE_ONEOF(c, Ephemeral) {
            isEvictable = c.isEvictable;
          }
        }
        if (isEvictable) {
          KJ_IF_SOME(a, actor) {
            KJ_IF_SOME(m, a->getHibernationManager()) {
              // The hibernation manager needs to survive actor eviction and be passed to the actor
              // constructor next time we create it.
              manager = m.addRef();
            }
          }
          shutdownTask =
              handleShutdown().eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
        }
      }

      kj::StringPtr getKey() {
        return key;
      }
      RequestTracker& getTracker() {
        return *tracker;
      }
      kj::Maybe<kj::Own<Worker::Actor::HibernationManager>> tryGetManagerRef() {
        return manager.map(
            [&](kj::Own<Worker::Actor::HibernationManager>& m) { return kj::addRef(*m); });
      }
      void updateAccessTime() {
        lastAccess = timer.now();
        KJ_IF_SOME(p, parent) {
          p.updateAccessTime();
        }
      }
      kj::TimePoint getLastAccess() {
        return lastAccess;
      }

      bool hasClients() {
        // If anyone holds a reference to the container other than the actor map, then it must be
        // a client.
        if (isShared()) return true;
        for (auto& facet: facets) {
          if (facet.value->hasClients()) return true;
        }
        return false;
      }
      kj::Own<ActorContainer> addRef() {
        return kj::addRef(*this);
      }

      // Get the actor, starting it if it's not already running.
      kj::Promise<kj::Own<Worker::Actor>> getActor() {
        requireNotBroken();

        if (actor == kj::none) {
          KJ_IF_SOME(promise, classAndId.tryGet<kj::ForkedPromise<void>>()) {
            co_await promise;
          }

          auto& [actorClass, id] = KJ_ASSERT_NONNULL(classAndId.tryGet<ClassAndId>());

          KJ_IF_SOME(promise, actorClass->whenReady()) {
            co_await promise;
          }

          // A concurrent request could have started the actor, so check again.
          if (actor == kj::none) {
            start(actorClass, id);
          }
        }

        co_return KJ_ASSERT_NONNULL(actor)->addRef();
      }

      kj::Promise<kj::Own<WorkerInterface>> startRequest(
          IoChannelFactory::SubrequestMetadata metadata) {
        auto actor = co_await getActor();

        if (ns.cleanupTask == kj::none) {
          // Need to start the cleanup loop.
          ns.cleanupTask = ns.cleanupLoop();
        }

        // Since `getActor()` completed, `classAndId` must be resolved.
        auto& actorClass = KJ_ASSERT_NONNULL(classAndId.tryGet<ClassAndId>()).actorClass;

        co_return actorClass->startRequest(kj::mv(metadata), kj::mv(actor))
            .attach(kj::defer([self = kj::addRef(*this)]() mutable { self->updateAccessTime(); }));
      }

      // Abort this actor, shutting it down.
      //
      // It is the caller's responsibility to ensure that the aborted ActorContainer has been
      // removed from any maps that would cause it to receive further traffic, since any further
      // requests will be expected to fail. abort() does NOT attempt to remove the ActorContainer
      // from the parent facet map since at most call sites it makes more sense to handle this
      // directly.
      void abort(kj::Maybe<const kj::Exception&> reason) {
        if (brokenReason != kj::none) return;

        KJ_IF_SOME(a, actor) {
          // Unknown broken reason.
          a->shutdown(0, reason);
        }

        for (auto& facet: facets) {
          facet.value->abort(reason);
        }

        onBrokenTask = kj::none;
        shutdownTask = kj::none;
        manager = kj::none;
        tracker->shutdown();
        actor = kj::none;
        containerClient = kj::none;

        KJ_IF_SOME(r, reason) {
          brokenReason = kj::cp(r);
        } else {
          brokenReason = JSG_KJ_EXCEPTION(FAILED, Error, "Actor aborted for uknown reason.");
        }
      }

      kj::Own<ActorContainer> getFacetContainer(
          kj::String childKey, kj::Function<kj::Promise<StartInfo>()> getStartInfo) {
        auto makeContainer = [&]() {
          auto promise = callFacetStartCallback(kj::mv(getStartInfo));
          return kj::refcounted<ActorContainer>(
              kj::mv(childKey), ns, *this, kj::mv(promise), timer);
        };

        bool isNew = false;

        auto& entry = facets.findOrCreateEntry(childKey, [&]() mutable {
          isNew = true;
          auto container = makeContainer();
          return ActorMap::Entry{container->getKey(), kj::mv(container)};
        });

        return entry.value->addRef();
      }

      kj::Own<IoChannelFactory::ActorChannel> getFacet(
          kj::StringPtr name, kj::Function<kj::Promise<StartInfo>()> getStartInfo) override {
        auto facet = getFacetContainer(kj::str(name), kj::mv(getStartInfo));
        return kj::refcounted<ActorChannelImpl>(kj::mv(facet));
      }

      void abortFacet(kj::StringPtr name, kj::Exception reason) override {
        KJ_IF_SOME(entry, facets.findEntry(name)) {
          entry.value->abort(reason);
          facets.erase(entry);
        }
      }

      void deleteFacet(kj::StringPtr name) override {
        // First, abort any running facets.
        abortFacet(name, JSG_KJ_EXCEPTION(FAILED, Error, "Facet was deleted."));

        // Then delete the underlying storage.
        KJ_IF_SOME(as, ns.actorStorage) {
          // Note that if there's no facet index then there couldn't possibly be any child storage.
          KJ_IF_SOME(index, getFacetTreeIndexIfNotEmpty()) {
            uint childId = index.getId(getFacetId(), name);
            deleteDescendantStorage(*as.directory, childId);
            as.directory->remove(getSqlitePathForId(childId));
          }
        }
      }

     private:
      // The actor is constructed after the ActorContainer so it starts off empty.
      kj::Maybe<kj::Own<Worker::Actor>> actor;

      kj::String key;
      kj::Own<RequestTracker> tracker;
      ActorNamespace& ns;
      ActorContainer& root;
      kj::Maybe<ActorContainer&> parent;
      kj::Timer& timer;
      kj::TimePoint lastAccess;
      kj::Maybe<kj::Own<Worker::Actor::HibernationManager>> manager;
      kj::Maybe<kj::Promise<void>> shutdownTask;
      kj::Maybe<kj::Promise<void>> onBrokenTask;
      kj::Maybe<kj::Exception> brokenReason;

      // Reference to the ContainerClient (if container is enabled for this actor)
      kj::Maybe<kj::Own<ContainerClient>> containerClient;

      // If this is a `ForkedPromise<void>`, await the promise. When it has resolved, then
      // `classAndId` will have been replaced with the resolved `ClassAndId` value.
      kj::OneOf<ClassAndId, kj::ForkedPromise<void>> classAndId;

      // FacetTreeIndex for this actor. Only initialized on the root.
      kj::Maybe<kj::Own<FacetTreeIndex>> facetTreeIndex;

      // ID of this facet. Initialized when getFacetId() is first called.
      kj::Maybe<uint> facetId;

      ActorMap facets;

      // Get the facet ID for this facet. The root facet always has ID zero, but all other facets
      // need to be looked up in the index to make sure they are assigned consistent IDs.
      uint getFacetId() {
        KJ_IF_SOME(f, facetId) {
          return f;
        }

        ActorContainer& parent = KJ_UNWRAP_OR(this->parent, return 0);

        FacetTreeIndex& index = root.ensureFacetTreeIndex();
        return index.getId(parent.getFacetId(), key);
      }

      // Get the facet tree index, opening the file if it hasn't been opened yet, and creating it
      // if it hasn't been created yet.
      FacetTreeIndex& ensureFacetTreeIndex() {
        KJ_REQUIRE(parent == kj::none, "only 'root' may ensureFacetTreeIndex()");

        KJ_IF_SOME(i, facetTreeIndex) {
          return *i;
        } else {
          // Facet tree index hasn't been initialized yet. Do that now (opening the existing file,
          // or creating it if it doesn't exist).
          auto& as = KJ_REQUIRE_NONNULL(
              ns.actorStorage, "can't call getFacetId() when there's no backing storage");
          auto indexFile = as.directory->openFile(
              kj::Path({kj::str(key, ".facets")}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
          return *facetTreeIndex.emplace(kj::heap<FacetTreeIndex>(kj::mv(indexFile)));
        }
      }

      // Like ensureFacetTreeIndex() but if the index doesn't exist on disk, return kj::none.
      kj::Maybe<FacetTreeIndex&> getFacetTreeIndexIfNotEmpty() {
        KJ_REQUIRE(parent == kj::none);

        KJ_IF_SOME(i, facetTreeIndex) {
          return *i;
        } else {
          // Facet tree index hasn't been initialized yet. If the file exists, open it. Otherwise,
          // assume empty and return none.
          auto& as = KJ_UNWRAP_OR(ns.actorStorage, return kj::none);
          auto indexFile = KJ_UNWRAP_OR(
              as.directory->tryOpenFile(kj::Path({kj::str(key, ".facets")}), kj::WriteMode::MODIFY),
              return kj::none);
          return *facetTreeIndex.emplace(kj::heap<FacetTreeIndex>(kj::mv(indexFile)));
        }
      }

      // Get the path to the facet's sqlite database, within the actor namespace directory.
      kj::Path getSqlitePathForId(uint id) {
        if (id == 0) {
          return kj::Path({kj::str(root.key, ".sqlite")});
        } else {
          return kj::Path({kj::str(root.key, '.', id, ".sqlite")});
        }
      }

      void deleteDescendantStorage(const kj::Directory& dir, uint parentId) {
        KJ_IF_SOME(index, getFacetTreeIndexIfNotEmpty()) {
          deleteDescendantStorage(dir, index, parentId);
        } else {
          // There's no index, so there must be no facets (other than the root).
          KJ_ASSERT(parentId == 0);
        }
      }

      void deleteDescendantStorage(const kj::Directory& dir, FacetTreeIndex& index, uint parentId) {
        index.forEachChild(parentId, [&](uint childId, kj::StringPtr childName) {
          deleteDescendantStorage(dir, index, childId);
          dir.remove(getSqlitePathForId(childId));
        });
      }

      void requireNotBroken() {
        KJ_IF_SOME(e, brokenReason) {
          kj::throwFatalException(kj::cp(e));
        }
      }

      kj::Promise<void> monitorOnBroken(Worker::Actor& actor) {
        try {
          // It's possible for this to never resolve if the actor never breaks,
          // in which case the returned promise will just be canceled.
          co_await actor.onBroken();
          KJ_FAIL_ASSERT("actor.onBroken() resolved normally?");
        } catch (...) {
          brokenReason = kj::getCaughtExceptionAsKj();
        }

        for (auto& facet: facets) {
          facet.value->abort(brokenReason);
        }
        facets.clear();

        // HACK: Dropping the ActorContainer will delete onBrokenTask, cancelling ourselves. This
        //   would crash. To avoid the problem, detach ourselves. This is safe because we know that
        //   once we return there's nothing left for this promise to do anyway.
        KJ_ASSERT_NONNULL(onBrokenTask).detach([](kj::Exception&& e) {});

        // Hollow out the object, so that if it still has references, they won't keep these parts
        // alive. Since any further calls to `getActor()` will throw, we don't have to worry about
        // the actor being recreated.
        auto actorToDrop = kj::mv(this->actor);
        tracker->shutdown();
        auto managerToDrop = kj::mv(manager);

        // Note that we remove the entire ActorContainer from the map -- this drops the
        // HibernationManager so any connected hibernatable websockets will be disconnected.
        KJ_IF_SOME(p, parent) {
          p.facets.erase(key);
        } else {
          ns.actors.erase(key);
        }

        // WARNING: `this` MAY HAVE BEEN DELETED as a result of the above `erase()`. Do not access
        //   it again here.
      }

      // Processes the eviction of the Durable Object and hibernates active websockets.
      kj::Promise<void> handleShutdown() {
        // After 10 seconds of inactivity, we destroy the Worker::Actor and hibernate any active
        // JS WebSockets.
        // TODO(someday): We could make this timeout configurable to make testing less burdensome.
        co_await timer.afterDelay(10 * kj::SECONDS);
        // Cancel the onBroken promise, since we're about to destroy the actor anyways and don't
        // want to trigger it.
        onBrokenTask = kj::none;
        KJ_IF_SOME(a, actor) {
          if (a->isShared()) {
            // Our ActiveRequest refcounting has broken somewhere. This is likely because we're
            // `addRef`-ing an actor that has had an ActiveRequest attached to its kj::Own (in other
            // words, the ActiveRequest count is less than it should be).
            //
            // Rather than dropping our actor and possibly ending up with split-brain,
            // we should opt out of the deferred proxy optimization and log the error to Sentry.
            KJ_LOG(ERROR,
                "Detected internal bug in hibernation: Durable Object has strong references "
                "when hibernation timeout expired.");

            co_return;
          }
          KJ_IF_SOME(m, manager) {
            auto& worker = a->getWorker();
            auto workerStrongRef = kj::atomicAddRef(worker);
            // Take an async lock, we can't use `takeAsyncLock(RequestObserver&)` since we don't
            // have an `IncomingRequest` at this point.
            //
            // Note that we do not have a race here because this is part of the `shutdownTask`
            // promise. If a new request comes in while we're waiting to get the lock then we will
            // cancel this promise.
            Worker::AsyncLock asyncLock = co_await worker.takeAsyncLockWithoutRequest(nullptr);
            workerStrongRef->runInLockScope(
                asyncLock, [&](Worker::Lock& lock) { m->hibernateWebSockets(lock); });
          }
          a->shutdown(
              0, KJ_EXCEPTION(DISCONNECTED, "broken.dropped; Actor freed due to inactivity"));
        }
        // Destroy the last strong Worker::Actor reference.
        actor = kj::none;

        // Drop our reference to the ContainerClient
        // If setInactivityTimeout() was called, the timer still holds a reference
        // so the container stays alive until the timeout expires
        containerClient = kj::none;
      }

      void start(kj::Own<ActorClass>& actorClass, Worker::Actor::Id& id) {
        KJ_REQUIRE(actor == nullptr);

        auto makeActorCache = [this](const ActorCache::SharedLru& sharedLru, OutputGate& outputGate,
                                  ActorCache::Hooks& hooks,
                                  SqliteObserver& sqliteObserver) mutable {
          return ns.config.tryGet<Durable>().map(
              [&](const Durable& d) -> kj::Own<ActorCacheInterface> {
            KJ_IF_SOME(as, ns.actorStorage) {
              kj::Own<ActorSqlite::Hooks> sqliteHooks;
              if (parent == kj::none) {
                KJ_IF_SOME(a, ns.alarmScheduler) {
                  sqliteHooks = kj::heap<ActorSqliteHooks>(
                      a, ActorKey{.uniqueKey = d.uniqueKey, .actorId = key});
                } else {
                  // No alarm scheduler available, use default hooks instance.
                  sqliteHooks = fakeOwn(ActorSqlite::Hooks::getDefaultHooks());
                }
              } else {
                // TODO(someday): Support alarms in facets, somehow.
                sqliteHooks = fakeOwn(ActorSqlite::Hooks::getDefaultHooks());
              }

              uint selfId = getFacetId();
              auto path = getSqlitePathForId(selfId);
              auto db = kj::heap<SqliteDatabase>(
                  as.vfs, kj::mv(path), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

              // Before we do anything, make sure the database is in WAL mode. We also need to
              // do this after reset() is used, so register a callback for that.
              db->run("PRAGMA journal_mode=WAL;");

              db->afterReset([this, &dir = *as.directory, selfId](SqliteDatabase& db) {
                db.run("PRAGMA journal_mode=WAL;");

                // reset() is used when the app called deleteAll(), in which case we also want to
                // delete all child facets.
                // TODO(someday): Arguably this should be transactional somehow so if we fail here
                //   we don't leave the facets still there after the parent has already been reset.
                //   But most filesystems do not support transactions, so we'd have to do something
                //   like store a flag in the parent DB saying "reset pending" so that on a restart
                //   we retry the deletions. Note that in production on SRS, this is actually
                //   transactional -- there's only a problem when running locally with workerd.
                deleteDescendantStorage(dir, selfId);
              });

              return kj::heap<ActorSqlite>(kj::mv(db), outputGate,
                  [](SpanParent) -> kj::Promise<void> { return kj::READY_NOW; }, *sqliteHooks)
                  .attach(kj::mv(sqliteHooks));
            } else {
              // Create an ActorCache backed by a fake, empty storage. Elsewhere, we configure
              // ActorCache never to flush, so this effectively creates in-memory storage.
              return kj::heap<ActorCache>(
                  newEmptyReadOnlyActorStorage(), sharedLru, outputGate, hooks);
            }
          });
        };

        bool enableSql = true;
        kj::Maybe<config::Worker::DurableObjectNamespace::ContainerOptions::Reader>
            containerOptions = kj::none;
        kj::Maybe<kj::StringPtr> uniqueKey;
        KJ_SWITCH_ONEOF(ns.config) {
          KJ_CASE_ONEOF(c, Durable) {
            enableSql = c.enableSql;
            containerOptions = c.containerOptions;
            uniqueKey = c.uniqueKey;
          }
          KJ_CASE_ONEOF(c, Ephemeral) {
            enableSql = c.enableSql;
          }
        }

        auto makeStorage =
            [enableSql = enableSql](jsg::Lock& js, const Worker::Api& api,
                ActorCacheInterface& actorCache) -> jsg::Ref<api::DurableObjectStorage> {
          return js.alloc<api::DurableObjectStorage>(
              js, IoContext::current().addObject(actorCache), enableSql);
        };

        auto loopback = kj::refcounted<Loopback>(*this);

        kj::Maybe<rpc::Container::Client> container = kj::none;
        KJ_IF_SOME(config, containerOptions) {
          KJ_ASSERT(config.hasImageName(), "Image name is required");
          auto imageName = config.getImageName();
          kj::String containerId;
          KJ_SWITCH_ONEOF(id) {
            KJ_CASE_ONEOF(globalId, kj::Own<ActorIdFactory::ActorId>) {
              containerId = globalId->toString();
            }
            KJ_CASE_ONEOF(existingId, kj::String) {
              containerId = kj::str(existingId);
            }
          }

          container = ns.getContainerClient(
              kj::str("workerd-", KJ_ASSERT_NONNULL(uniqueKey), "-", containerId), imageName);
        }

        auto actor = actorClass->newActor(getTracker(), Worker::Actor::cloneId(id),
            kj::mv(makeActorCache), kj::mv(makeStorage), kj::mv(loopback), tryGetManagerRef(),
            kj::mv(container), *this);
        onBrokenTask = monitorOnBroken(*actor);
        this->actor = kj::mv(actor);
      }

      // Helper coroutine to call `getStartInfo()`, the start callback for a facet, while making
      // sure the function stays alive until the returned promise resolves.
      static kj::Promise<ClassAndId> callFacetStartCallback(
          kj::Function<kj::Promise<StartInfo>()> getStartInfo) {
        auto info = co_await getStartInfo();
        co_return ClassAndId(info.actorClass.downcast<ActorClass>(), kj::mv(info.id));
      }
    };

    kj::Own<ActorContainer> getActorContainer(Worker::Actor::Id id) {
      kj::String key;

      KJ_SWITCH_ONEOF(id) {
        KJ_CASE_ONEOF(obj, kj::Own<ActorIdFactory::ActorId>) {
          KJ_REQUIRE(config.is<Durable>());
          key = obj->toString();
        }
        KJ_CASE_ONEOF(str, kj::String) {
          KJ_REQUIRE(config.is<Ephemeral>());
          key = kj::str(str);
        }
      }

      return actors
          .findOrCreate(key, [&]() mutable {
        auto container = kj::refcounted<ActorContainer>(kj::mv(key), *this, kj::none,
            ActorContainer::ClassAndId(kj::addRef(*actorClass), kj::mv(id)), timer);

        return kj::HashMap<kj::StringPtr, kj::Own<ActorContainer>>::Entry{
          container->getKey(), kj::mv(container)};
      })->addRef();
    }

    kj::Own<ContainerClient> getContainerClient(
        kj::StringPtr containerId, kj::StringPtr imageName) {
      KJ_IF_SOME(existingClient, containerClients.find(containerId)) {
        return existingClient->addRef();
      }

      // No existing container in the map, create a new one
      auto& dockerPathRef = KJ_ASSERT_NONNULL(
          dockerPath, "dockerPath must be defined to enable containers on this Durable Object.");

      // Remove from the map when the container is destroyed
      kj::Function<void()> cleanupCallback = [this, containerId = kj::str(containerId)]() {
        containerClients.erase(containerId);
      };

      auto client = kj::refcounted<ContainerClient>(byteStreamFactory, timer, dockerNetwork,
          kj::str(dockerPathRef), kj::str(containerId), kj::str(imageName),
          containerEgressInterceptorImage.map([](kj::StringPtr s) { return kj::str(s); }),
          waitUntilTasks, kj::mv(cleanupCallback), channelTokenHandler);

      // Store raw pointer in map (does not own)
      containerClients.insert(kj::str(containerId), client.get());

      return kj::mv(client);
    }

    void abortAll(kj::Maybe<const kj::Exception&> reason) {
      for (auto& actor: actors) {
        actor.value->abort(reason);
      }
      actors.clear();
    }

   private:
    kj::Own<ActorClass> actorClass;
    const ActorConfig& config;

    struct ActorStorage {
      kj::Own<const kj::Directory> directory;
      SqliteDatabase::Vfs vfs;

      ActorStorage(kj::Own<const kj::Directory> directoryParam)
          : directory(kj::mv(directoryParam)),
            vfs(*directory) {}
    };

    // Note: The Vfs must not be torn down until all actors have been torn down, so we have to
    //   declare `actorStorage` before `actors`.
    kj::Maybe<ActorStorage> actorStorage;

    // If the actor is broken, we remove it from the map. However, if it's just evicted due to
    // inactivity, we keep the ActorContainer in the map but drop the Own<Worker::Actor>. When a new
    // request comes in, we recreate the Own<Worker::Actor>.
    ActorMap actors;

    // Map of container IDs to ContainerClients (for reconnection support with inactivity timeouts).
    // The map holds raw pointers (not ownership) - ContainerClients are owned by actors and timers.
    // When the last reference is dropped, the destructor removes the entry from this map.
    kj::HashMap<kj::String, ContainerClient*> containerClients;

    kj::Maybe<kj::Promise<void>> cleanupTask;
    kj::Timer& timer;
    capnp::ByteStreamFactory& byteStreamFactory;
    ChannelTokenHandler& channelTokenHandler;
    kj::Network& dockerNetwork;
    kj::Maybe<kj::StringPtr> dockerPath;
    kj::Maybe<kj::StringPtr> containerEgressInterceptorImage;
    kj::TaskSet& waitUntilTasks;
    kj::Maybe<AlarmScheduler&> alarmScheduler;

    // Removes actors from `actors` after 70 seconds of last access.
    kj::Promise<void> cleanupLoop() {
      constexpr auto EXPIRATION = 70 * kj::SECONDS;

      // Don't bother running the loop if the config doesn't allow eviction.
      KJ_SWITCH_ONEOF(config) {
        KJ_CASE_ONEOF(c, Durable) {
          if (!c.isEvictable) co_return;
        }
        KJ_CASE_ONEOF(c, Ephemeral) {
          if (!c.isEvictable) co_return;
        }
      }

      while (true) {
        auto now = timer.now();
        actors.eraseAll([&](auto&, kj::Own<ActorContainer>& entry) {
          // Check getLastAccess() before hasClients() since it's faster.
          if ((now - entry->getLastAccess()) <= EXPIRATION) {
            // Used recently; don't evict.
            return false;
          }

          if (entry->hasClients()) {
            // There's still an active client; don't evict.
            return false;
          }

          // No clients and not used in a while, evict this actor.
          return true;
        });

        co_await timer.atTime(now + EXPIRATION);
      }
    }

    // Implements actor loopback, which is used by websocket hibernation to deliver events to the
    // actor from the websocket's read loop.
    class Loopback: public Worker::Actor::Loopback, public kj::Refcounted {
     public:
      Loopback(ActorContainer& actorContainer): actorContainer(actorContainer) {}

      kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata metadata) override {
        return newPromisedWorkerInterface(actorContainer.startRequest(kj::mv(metadata)));
      }

      kj::Own<Worker::Actor::Loopback> addRef() override {
        return kj::addRef(*this);
      }

     private:
      ActorContainer& actorContainer;
    };

    class ActorSqliteHooks final: public ActorSqlite::Hooks {
     public:
      ActorSqliteHooks(AlarmScheduler& alarmScheduler, ActorKey actor)
          : alarmScheduler(alarmScheduler),
            actor(actor) {}

      // We ignore the priorTask in workerd because everything should run synchronously.
      kj::Promise<void> scheduleRun(
          kj::Maybe<kj::Date> newAlarmTime, kj::Promise<void> priorTask) override {
        KJ_IF_SOME(scheduledTime, newAlarmTime) {
          alarmScheduler.setAlarm(actor, scheduledTime);
        } else {
          alarmScheduler.deleteAlarm(actor);
        }
        return kj::READY_NOW;
      }

     private:
      AlarmScheduler& alarmScheduler;
      ActorKey actor;
    };
  };

 private:
  class EntrypointService final: public Service {
   public:
    EntrypointService(WorkerService& worker,
        kj::Maybe<kj::StringPtr> entrypoint,
        kj::Maybe<Frankenvalue> props,
        const kj::HashSet<kj::String>& handlers)
        : worker(kj::addRef(worker)),
          entrypoint(entrypoint),
          handlers(handlers),
          props(kj::mv(props)) {}

    kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
      return startRequest(kj::mv(metadata), false);
    }

    kj::Own<WorkerInterface> startRequest(
        IoChannelFactory::SubrequestMetadata metadata, bool isTracer) {
      Frankenvalue props;
      KJ_IF_SOME(p, this->props) {
        props = p.clone();
      } else {
        // Calling ctx.exports loopback without specifying props. Use empty props.
      }
      return worker->startRequest(kj::mv(metadata), entrypoint, kj::mv(props), kj::none, isTracer);
    }

    bool hasHandler(kj::StringPtr handlerName) override {
      return handlers.contains(handlerName);
    }

    // Return underlying WorkerService.
    virtual Service* service() override {
      return worker;
    }

    kj::Own<Service> forProps(Frankenvalue props) override {
      if (this->props != kj::none) {
        // This entrypoint is already specialized. Delegate to the default implementation (which
        // will throw an exception).
        return Service::forProps(kj::mv(props));
      }

      return kj::refcounted<EntrypointService>(*worker, entrypoint, kj::mv(props), handlers);
    }

    void requireAllowsTransfer() override {
      worker->requireAllowsTransfer();
    }

    kj::Array<byte> getToken(ChannelTokenUsage usage) override {
      worker->requireAllowsTransfer();

      // If requireAllowsTransfer() passed, then we are not dynamic so should have a service name.
      // Unspecialized loopback entrypoints are not serializable, so if we get here we must have
      // props.
      return worker->channelTokenHandler.encodeSubrequestChannelToken(
          usage, KJ_ASSERT_NONNULL(worker->serviceName), entrypoint, KJ_ASSERT_NONNULL(props));
    }

   private:
    kj::Own<WorkerService> worker;
    kj::Maybe<kj::StringPtr> entrypoint;
    const kj::HashSet<kj::String>& handlers;
    kj::Maybe<Frankenvalue> props;
  };

  class ActorClassImpl final: public ActorClass {
   public:
    ActorClassImpl(WorkerService& service, kj::StringPtr className, kj::Maybe<Frankenvalue> props)
        : service(kj::addRef(service)),
          className(className),
          props(kj::mv(props)) {}

    void requireAllowsTransfer() override {
      service->requireAllowsTransfer();
    }

    kj::Own<Worker::Actor> newActor(kj::Maybe<RequestTracker&> tracker,
        Worker::Actor::Id actorId,
        Worker::Actor::MakeActorCacheFunc makeActorCache,
        Worker::Actor::MakeStorageFunc makeStorage,
        kj::Own<Worker::Actor::Loopback> loopback,
        kj::Maybe<kj::Own<Worker::Actor::HibernationManager>> manager,
        kj::Maybe<rpc::Container::Client> container,
        kj::Maybe<Worker::Actor::FacetManager&> facetManager) override {
      TimerChannel& timerChannel = *service;

      // We define this event ID in the internal codebase, but to have WebSocket Hibernation
      // work for local development we need to pass an event type.
      static constexpr uint16_t hibernationEventTypeId = 8;

      Frankenvalue props;
      KJ_IF_SOME(p, this->props) {
        props = p.clone();
      } else {
        // Using ctx.exports class loopback without specifying props. Use empty props.
      }

      return kj::refcounted<Worker::Actor>(*service->worker, tracker, kj::mv(actorId), true,
          kj::mv(makeActorCache), className, kj::mv(props), kj::mv(makeStorage), kj::mv(loopback),
          timerChannel, kj::refcounted<ActorObserver>(), kj::mv(manager), hibernationEventTypeId,
          kj::mv(container), facetManager);
    }

    kj::Own<WorkerInterface> startRequest(
        IoChannelFactory::SubrequestMetadata metadata, kj::Own<Worker::Actor> actor) override {
      // The `props` parameter is empty here because props are not passed per-request, they are
      // passed at Actor construction time.
      return service->startRequest(kj::mv(metadata), className, {}, kj::mv(actor));
    }

    kj::Own<ActorClass> forProps(Frankenvalue props) override {
      if (this->props != kj::none) {
        // This entrypoint is already specialized. Delegate to the default implementation (which
        // will throw an exception).
        return ActorClass::forProps(kj::mv(props));
      }

      return kj::refcounted<ActorClassImpl>(*service, className, kj::mv(props));
    }

    kj::Array<byte> getToken(ChannelTokenUsage usage) override {
      service->requireAllowsTransfer();

      // If requireAllowsTransfer() passed, then we are not dynamic so should have a service name.
      // Unspecialized loopback entrypoints are not serializable, so if we get here we must have
      // props.
      return service->channelTokenHandler.encodeActorClassChannelToken(
          usage, KJ_ASSERT_NONNULL(service->serviceName), className, KJ_ASSERT_NONNULL(props));
    }

   private:
    kj::Own<WorkerService> service;
    kj::StringPtr className;
    kj::Maybe<Frankenvalue> props;
  };

  ChannelTokenHandler& channelTokenHandler;

  // This service's name as defined in the original config, or null if it's a dynamic isolate.
  // Used only for serialization.
  kj::Maybe<kj::StringPtr> serviceName;

  ThreadContext& threadContext;
  const kj::MonotonicClock& monotonicClock;

  // LinkedIoChannels owns the SqliteDatabase::Vfs, so make sure it is destroyed last.
  kj::OneOf<LinkCallback, LinkedIoChannels> ioChannels;

  kj::Own<const Worker> worker;
  kj::Maybe<kj::HashSet<kj::String>> defaultEntrypointHandlers;
  kj::HashMap<kj::String, kj::HashSet<kj::String>> namedEntrypoints;
  kj::HashSet<kj::String> actorClassEntrypoints;
  kj::HashMap<kj::StringPtr, kj::Own<ActorNamespace>> actorNamespaces;
  kj::TaskSet waitUntilTasks;
  AbortActorsCallback abortActorsCallback;
  kj::Maybe<kj::String> dockerPath;
  kj::Maybe<kj::String> containerEgressInterceptorImage;
  bool isDynamic;

  class ActorChannelImpl final: public IoChannelFactory::ActorChannel {
   public:
    ActorChannelImpl(kj::Own<ActorNamespace::ActorContainer> actorContainer)
        : actorContainer(kj::mv(actorContainer)) {}
    ~ActorChannelImpl() noexcept(false) {
      actorContainer->updateAccessTime();
    }

    kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
      return newPromisedWorkerInterface(actorContainer->startRequest(kj::mv(metadata)));
    }

   private:
    kj::Own<ActorNamespace::ActorContainer> actorContainer;
  };

  // ---------------------------------------------------------------------------
  // implements kj::TaskSet::ErrorHandler

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, exception);
  }

  // ---------------------------------------------------------------------------
  // implements IoChannelFactory

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    auto& channels =
        KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(), "link() has not been called");

    KJ_REQUIRE(channel < channels.subrequest.size(), "invalid subrequest channel number");
    return channels.subrequest[channel]->startRequest(kj::mv(metadata));
  }

  capnp::Capability::Client getCapability(uint channel) override {
    KJ_FAIL_REQUIRE("no capability channels");
  }
  class CacheClientImpl final: public CacheClient {
   public:
    CacheClientImpl(
        IoChannelFactory::SubrequestChannel& cacheService, kj::HttpHeaderId cacheNamespaceHeader)
        : cacheService(kj::addRef(cacheService)),
          cacheNamespaceHeader(cacheNamespaceHeader) {}

    kj::Own<kj::HttpClient> getDefault(CacheClient::SubrequestMetadata metadata) override {
      return kj::heap<CacheHttpClientImpl>(*cacheService, cacheNamespaceHeader, kj::none,
          kj::mv(metadata.cfBlobJson), kj::mv(metadata.parentSpan));
    }

    kj::Own<kj::HttpClient> getNamespace(
        kj::StringPtr cacheName, CacheClient::SubrequestMetadata metadata) override {
      auto encodedName = kj::encodeUriComponent(cacheName);
      return kj::heap<CacheHttpClientImpl>(*cacheService, cacheNamespaceHeader, kj::mv(encodedName),
          kj::mv(metadata.cfBlobJson), kj::mv(metadata.parentSpan));
    }

   private:
    kj::Own<IoChannelFactory::SubrequestChannel> cacheService;
    kj::HttpHeaderId cacheNamespaceHeader;
  };

  class CacheHttpClientImpl final: public kj::HttpClient {
   public:
    CacheHttpClientImpl(IoChannelFactory::SubrequestChannel& parent,
        kj::HttpHeaderId cacheNamespaceHeader,
        kj::Maybe<kj::String> cacheName,
        kj::Maybe<kj::String> cfBlobJson,
        SpanParent parentSpan)
        : client(asHttpClient(parent.startRequest({kj::mv(cfBlobJson), kj::mv(parentSpan)}))),
          cacheName(kj::mv(cacheName)),
          cacheNamespaceHeader(cacheNamespaceHeader) {}

    Request request(kj::HttpMethod method,
        kj::StringPtr url,
        const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize = kj::none) override {

      return client->request(method, url, addCacheNameHeader(headers, cacheName), expectedBodySize);
    }

   private:
    kj::Own<kj::HttpClient> client;
    kj::Maybe<kj::String> cacheName;
    kj::HttpHeaderId cacheNamespaceHeader;

    kj::HttpHeaders addCacheNameHeader(
        const kj::HttpHeaders& headers, kj::Maybe<kj::StringPtr> cacheName) {
      auto headersCopy = headers.cloneShallow();
      KJ_IF_SOME(name, cacheName) {
        headersCopy.setPtr(cacheNamespaceHeader, name);
      }

      return headersCopy;
    }
  };

  kj::Own<CacheClient> getCache() override {
    auto& channels =
        KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(), "link() has not been called");
    auto& cache = *JSG_REQUIRE_NONNULL(channels.cache, Error, "No Cache was configured");
    return kj::heap<CacheClientImpl>(cache, threadContext.getHeaderIds().cfCacheNamespace);
  }

  TimerChannel& getTimer() override {
    return *this;
  }

  kj::Promise<void> writeLogfwdr(
      uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
    auto& context = IoContext::current();

    auto headers = kj::HttpHeaders(context.getHeaderTable());
    auto client = context.getHttpClient(channel, true, kj::none, "writeLogfwdr"_kjc);

    auto urlStr = kj::str("https://fake-host");

    capnp::MallocMessageBuilder requestMessage;
    auto requestBuilder = requestMessage.initRoot<capnp::AnyPointer>();

    buildMessage(requestBuilder);
    capnp::JsonCodec json;
    auto requestJson = json.encode(requestBuilder.getAs<api::AnalyticsEngineEvent>());

    co_await context.waitForOutputLocks();

    auto innerReq = client->request(kj::HttpMethod::POST, urlStr, headers, requestJson.size());
    auto request = attachToRequest(kj::mv(innerReq), kj::refcountedWrapper(kj::mv(client)));

    co_await request.body->write(requestJson.asBytes())
        .attach(kj::mv(requestJson), kj::mv(request.body));
    auto response = co_await request.response;

    KJ_REQUIRE(response.statusCode >= 200 && response.statusCode < 300,
        "writeLogfwdr request returned an error");
    co_await response.body->readAllBytes().attach(kj::mv(response.body)).ignoreResult();
    co_return;
  }

  kj::Own<SubrequestChannel> getSubrequestChannel(uint channel,
      kj::Maybe<Frankenvalue> props,
      kj::Maybe<VersionRequest> versionRequest) override {
    auto& channels =
        KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(), "link() has not been called");

    KJ_REQUIRE(channel < channels.subrequest.size(), "invalid subrequest channel number");

    SubrequestChannel& channelRef = *channels.subrequest[channel];

    KJ_IF_SOME(p, props) {
      // Requesting specialization of loopback (ctx.exports) entrypoint with props.
      auto& service = KJ_REQUIRE_NONNULL(kj::dynamicDowncastIfAvailable<Service>(channelRef),
          "referenced channel is not a loopback channel");
      return service.forProps(kj::mv(p));
    }

    return kj::addRef(channelRef);
  }

  kj::Own<ActorChannel> getGlobalActor(uint channel,
      const ActorIdFactory::ActorId& id,
      kj::Maybe<kj::String> locationHint,
      ActorGetMode mode,
      bool enableReplicaRouting,
      ActorRoutingMode routingMode,
      SpanParent parentSpan,
      kj::Maybe<ActorVersion> version) override {
    JSG_REQUIRE(mode == ActorGetMode::GET_OR_CREATE, Error,
        "workerd only supports GET_OR_CREATE mode for getting actor stubs");
    JSG_REQUIRE(!enableReplicaRouting, Error, "workerd does not support replica routing.");
    JSG_REQUIRE(routingMode == ActorRoutingMode::DEFAULT, Error,
        "workerd does not support replica routing.");
    auto& channels =
        KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(), "link() has not been called");

    KJ_REQUIRE(channel < channels.actor.size(), "invalid actor channel number");
    auto& ns = JSG_REQUIRE_NONNULL(
        channels.actor[channel], Error, "Actor namespace configuration was invalid.");
    KJ_REQUIRE(ns.getConfig().is<Durable>());  // should have been verified earlier
    return ns.getActorChannel(id.clone());
  }

  kj::Own<ActorChannel> getColoLocalActor(
      uint channel, kj::StringPtr id, SpanParent parentSpan) override {
    auto& channels =
        KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(), "link() has not been called");

    KJ_REQUIRE(channel < channels.actor.size(), "invalid actor channel number");
    auto& ns = JSG_REQUIRE_NONNULL(
        channels.actor[channel], Error, "Actor namespace configuration was invalid.");
    KJ_REQUIRE(ns.getConfig().is<Ephemeral>());  // should have been verified earlier
    return ns.getActorChannel(kj::str(id));
  }

  kj::Own<ActorClassChannel> getActorClass(uint channel, kj::Maybe<Frankenvalue> props) override {
    auto& channels =
        KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(), "link() has not been called");

    KJ_REQUIRE(channel < channels.actorClass.size(), "invalid actor class channel number");

    ActorClass& cls = *channels.actorClass[channel];

    KJ_IF_SOME(p, props) {
      return cls.forProps(kj::mv(p));
    }

    return kj::addRef(cls);
  }

  void abortAllActors(kj::Maybe<kj::Exception&> reason) override {
    abortActorsCallback(reason);
  }

  kj::Own<WorkerStubChannel> loadIsolate(uint loaderChannel,
      kj::Maybe<kj::String> name,
      kj::Function<kj::Promise<DynamicWorkerSource>()> fetchSource) override;

  kj::Network& getWorkerdDebugPortNetwork() override {
    auto& channels =
        KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(), "link() has not been called");
    return KJ_REQUIRE_NONNULL(channels.workerdDebugPortNetwork,
        "workerdDebugPort binding is not enabled for this worker");
  }

  kj::Own<SubrequestChannel> subrequestChannelFromToken(
      ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) override {
    return channelTokenHandler.decodeSubrequestChannelToken(usage, token);
  }

  kj::Own<ActorClassChannel> actorClassFromToken(
      ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) override {
    return channelTokenHandler.decodeActorClassChannelToken(usage, token);
  }

  // ---------------------------------------------------------------------------
  // implements TimerChannel

  void syncTime() override {
    // Nothing to do
  }

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::systemPreciseCalendarClock().now();
  }

  kj::Promise<void> atTime(kj::Date when) override {
    auto delay = when - now(kj::none);
    // We can't use `afterDelay(delay)` here because kj::Timer::afterDelay() is equivalent to
    // `atTime(timer.now() + delay)`, and kj::Timer::now() only advances when the event loop
    // polls for I/O. If JavaScript executed for a significant amount of time since the last
    // poll (e.g. compiling/running a script before the first setTimeout), timer.now() will be
    // stale and the delay will effectively be shortened by that staleness, causing the timer
    // to fire too early. Instead, we compute the target time using a fresh reading from the
    // monotonic clock so the delay is measured from the actual present.
    return threadContext.getUnsafeTimer().atTime(monotonicClock.now() + delay);
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration t) override {
    return threadContext.getUnsafeTimer().afterDelay(t);
  }

  // ---------------------------------------------------------------------------
  // implements LimitEnforcer
  //
  // No limits are enforced.

  kj::Own<void> enterJs(jsg::Lock& lock, IoContext& context) override {
    return {};
  }
  void topUpActor() override {}
  void newSubrequest(bool isInHouse) override {}
  void newKvRequest(KvOpType op) override {}
  void newAnalyticsEngineRequest() override {}
  kj::Promise<void> limitDrain() override {
    return kj::NEVER_DONE;
  }
  kj::Promise<void> limitScheduled() override {
    return kj::NEVER_DONE;
  }
  kj::Duration getAlarmLimit() override {
    return 15 * kj::MINUTES;
  }
  size_t getBufferingLimit() override {
    return kj::maxValue;
  }
  kj::Maybe<EventOutcome> getLimitsExceeded() override {
    return kj::none;
  }
  kj::Promise<void> onLimitsExceeded() override {
    return kj::NEVER_DONE;
  }
  void setCpuLimitNearlyExceededCallback(kj::Function<void(void)> cb) override {}
  void requireLimitsNotExceeded() override {}
  void reportMetrics(RequestObserver& requestMetrics) override {}
  kj::Duration consumeTimeElapsedForPeriodicLogging() override {
    return 0 * kj::SECONDS;
  }
};

struct FutureSubrequestChannel {
  kj::OneOf<config::ServiceDesignator::Reader, kj::Own<IoChannelFactory::SubrequestChannel>>
      designator;
  kj::String errorContext;

  kj::Own<IoChannelFactory::SubrequestChannel> lookup(Server& server) && {
    KJ_SWITCH_ONEOF(designator) {
      KJ_CASE_ONEOF(conf, config::ServiceDesignator::Reader) {
        return server.lookupService(conf, kj::mv(errorContext));
      }
      KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::SubrequestChannel>) {
        return kj::mv(channel);
      }
    }
    KJ_UNREACHABLE;
  }
};

struct FutureActorChannel {
  config::Worker::Binding::DurableObjectNamespaceDesignator::Reader designator;
  kj::String errorContext;
};

struct FutureActorClassChannel {
  kj::OneOf<config::ServiceDesignator::Reader, kj::Own<Server::ActorClass>> designator;
  kj::String errorContext;

  kj::Own<Server::ActorClass> lookup(Server& server) && {
    KJ_SWITCH_ONEOF(designator) {
      KJ_CASE_ONEOF(conf, config::ServiceDesignator::Reader) {
        return server.lookupActorClass(conf, kj::mv(errorContext));
      }
      KJ_CASE_ONEOF(channel, kj::Own<Server::ActorClass>) {
        return kj::mv(channel);
      }
    }
    KJ_UNREACHABLE;
  }
};

struct FutureWorkerLoaderChannel {
  kj::String name;  // for error logging, not necessarily unique
  kj::Maybe<kj::String> id;
};

static kj::Maybe<WorkerdApi::Global> createBinding(kj::StringPtr workerName,
    config::Worker::Reader conf,
    config::Worker::Binding::Reader binding,
    Worker::ValidationErrorReporter& errorReporter,
    kj::Vector<FutureSubrequestChannel>& subrequestChannels,
    kj::Vector<FutureActorChannel>& actorChannels,
    kj::Vector<FutureActorClassChannel>& actorClassChannels,
    kj::Vector<FutureWorkerLoaderChannel>& workerLoaderChannels,
    bool& hasWorkerdDebugPortBinding,
    kj::HashMap<kj::String, kj::HashMap<kj::String, Server::ActorConfig>>& actorConfigs,
    bool experimental) {
  // creates binding object or returns null and reports an error
  using Global = WorkerdApi::Global;
  kj::StringPtr bindingName = binding.getName();
  TRACE_EVENT("workerd", "Server::WorkerService::createBinding()", "name", workerName.cStr(),
      "binding", bindingName.cStr());
  auto makeGlobal = [&](auto&& value) {
    return Global{.name = kj::str(bindingName), .value = kj::mv(value)};
  };

  auto errorContext = kj::str("Worker \"", workerName, "\"'s binding \"", bindingName, "\"");

  switch (binding.which()) {
    case config::Worker::Binding::UNSPECIFIED:
      errorReporter.addError(kj::str(errorContext, " does not specify any binding value."));
      return kj::none;

    case config::Worker::Binding::PARAMETER:
      KJ_UNIMPLEMENTED("TODO(beta): parameters");

    case config::Worker::Binding::TEXT:
      return makeGlobal(kj::str(binding.getText()));
    case config::Worker::Binding::DATA:
      return makeGlobal(kj::heapArray<byte>(binding.getData()));
    case config::Worker::Binding::JSON:
      return makeGlobal(Global::Json{kj::str(binding.getJson())});

    case config::Worker::Binding::WASM_MODULE:
      if (conf.isServiceWorkerScript()) {
        // Already handled earlier.
      } else {
        errorReporter.addError(kj::str(errorContext,
            " is a Wasm binding, but Wasm bindings are not allowed in "
            "modules-based scripts. Use Wasm modules instead."));
      }
      return kj::none;

    case config::Worker::Binding::CRYPTO_KEY: {
      auto keyConf = binding.getCryptoKey();
      Global::CryptoKey keyGlobal;

      switch (keyConf.which()) {
        case config::Worker::Binding::CryptoKey::RAW:
          keyGlobal.format = kj::str("raw");
          keyGlobal.keyData = kj::heapArray<kj::byte>(keyConf.getRaw());
          goto validFormat;
        case config::Worker::Binding::CryptoKey::HEX: {
          keyGlobal.format = kj::str("raw");
          auto decoded = kj::decodeHex(keyConf.getHex());
          if (decoded.hadErrors) {
            errorReporter.addError(
                kj::str("CryptoKey binding \"", binding.getName(), "\" contained invalid hex."));
          }
          keyGlobal.keyData = kj::Array<byte>(kj::mv(decoded));
          goto validFormat;
        }
        case config::Worker::Binding::CryptoKey::BASE64: {
          keyGlobal.format = kj::str("raw");
          auto decoded = kj::decodeBase64(keyConf.getBase64());
          if (decoded.hadErrors) {
            errorReporter.addError(
                kj::str("CryptoKey binding \"", binding.getName(), "\" contained invalid base64."));
          }
          keyGlobal.keyData = kj::Array<byte>(kj::mv(decoded));
          goto validFormat;
        }
        case config::Worker::Binding::CryptoKey::PKCS8: {
          keyGlobal.format = kj::str("pkcs8");
          auto pem = KJ_UNWRAP_OR(decodePem(keyConf.getPkcs8()), {
            errorReporter.addError(kj::str(
                "CryptoKey binding \"", binding.getName(), "\" contained invalid PEM format."));
            return kj::none;
          });
          if (pem.type != "PRIVATE KEY") {
            errorReporter.addError(kj::str("CryptoKey binding \"", binding.getName(),
                "\" contained wrong PEM type, "
                "expected \"PRIVATE KEY\" but got \"",
                pem.type, "\"."));
            return kj::none;
          }
          keyGlobal.keyData = kj::mv(pem.data);
          goto validFormat;
        }
        case config::Worker::Binding::CryptoKey::SPKI: {
          keyGlobal.format = kj::str("spki");
          auto pem = KJ_UNWRAP_OR(decodePem(keyConf.getSpki()), {
            errorReporter.addError(kj::str(
                "CryptoKey binding \"", binding.getName(), "\" contained invalid PEM format."));
            return kj::none;
          });
          if (pem.type != "PUBLIC KEY") {
            errorReporter.addError(kj::str("CryptoKey binding \"", binding.getName(),
                "\" contained wrong PEM type, "
                "expected \"PUBLIC KEY\" but got \"",
                pem.type, "\"."));
            return kj::none;
          }
          keyGlobal.keyData = kj::mv(pem.data);
          goto validFormat;
        }
        case config::Worker::Binding::CryptoKey::JWK:
          keyGlobal.format = kj::str("jwk");
          keyGlobal.keyData = Global::Json{kj::str(keyConf.getJwk())};
          goto validFormat;
      }
      errorReporter.addError(kj::str("Encountered unknown CryptoKey type for binding \"",
          binding.getName(), "\". Was the config compiled with a newer version of the schema?"));
      return kj::none;
    validFormat:

      auto algorithmConf = keyConf.getAlgorithm();
      switch (algorithmConf.which()) {
        case config::Worker::Binding::CryptoKey::Algorithm::NAME:
          keyGlobal.algorithm = Global::Json{escapeJsonString(algorithmConf.getName())};
          goto validAlgorithm;
        case config::Worker::Binding::CryptoKey::Algorithm::JSON:
          keyGlobal.algorithm = Global::Json{kj::str(algorithmConf.getJson())};
          goto validAlgorithm;
      }
      errorReporter.addError(kj::str("Encountered unknown CryptoKey algorithm type for binding \"",
          binding.getName(), "\". Was the config compiled with a newer version of the schema?"));
      return kj::none;
    validAlgorithm:

      keyGlobal.extractable = keyConf.getExtractable();
      keyGlobal.usages = KJ_MAP(usage, keyConf.getUsages()) { return kj::str(usage); };

      return makeGlobal(kj::mv(keyGlobal));
      return kj::none;
    }

    case config::Worker::Binding::SERVICE: {
      uint channel = static_cast<uint>(subrequestChannels.size()) +
          IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel{binding.getService(), kj::mv(errorContext)});
      return makeGlobal(
          Global::Fetcher{.channel = channel, .requiresHost = true, .isInHouse = false});
    }

    case config::Worker::Binding::DURABLE_OBJECT_NAMESPACE: {
      auto actorBinding = binding.getDurableObjectNamespace();
      const Server::ActorConfig* actorConfig;
      if (actorBinding.hasServiceName()) {
        auto& svcMap = KJ_UNWRAP_OR(actorConfigs.find(actorBinding.getServiceName()), {
          errorReporter.addError(kj::str(errorContext, " refers to a service \"",
              actorBinding.getServiceName(), "\", but no such service is defined."));
          return kj::none;
        });

        actorConfig = &KJ_UNWRAP_OR(svcMap.find(actorBinding.getClassName()), {
          errorReporter.addError(
              kj::str(errorContext, " refers to a Durable Object namespace named \"",
                  actorBinding.getClassName(), "\" in service \"", actorBinding.getServiceName(),
                  "\", but no such Durable Object namespace is defined by that service."));
          return kj::none;
        });
      } else {
        auto& localActorConfigs = KJ_ASSERT_NONNULL(actorConfigs.find(workerName));
        actorConfig = &KJ_UNWRAP_OR(localActorConfigs.find(actorBinding.getClassName()), {
          errorReporter.addError(kj::str(errorContext,
              " refers to a Durable Object namespace named \"", actorBinding.getClassName(),
              "\", but no such Durable Object namespace is defined "
              "by this Worker."));
          return kj::none;
        });
      }

      uint channel = static_cast<uint>(actorChannels.size());
      actorChannels.add(FutureActorChannel{actorBinding, kj::mv(errorContext)});

      KJ_SWITCH_ONEOF(*actorConfig) {
        KJ_CASE_ONEOF(durable, Server::Durable) {
          return makeGlobal(Global::DurableActorNamespace{
            .actorChannel = channel, .uniqueKey = durable.uniqueKey});
        }
        KJ_CASE_ONEOF(_, Server::Ephemeral) {
          return makeGlobal(Global::EphemeralActorNamespace{.actorChannel = channel});
        }
      }

      return kj::none;
    }

    case config::Worker::Binding::KV_NAMESPACE: {
      uint channel = static_cast<uint>(subrequestChannels.size()) +
          IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(
          FutureSubrequestChannel{binding.getKvNamespace(), kj::mv(errorContext)});

      return makeGlobal(Global::KvNamespace{
        .subrequestChannel = channel, .bindingName = kj::str(binding.getName())});
    }

    case config::Worker::Binding::R2_BUCKET: {
      uint channel = static_cast<uint>(subrequestChannels.size()) +
          IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel{binding.getR2Bucket(), kj::mv(errorContext)});
      return makeGlobal(Global::R2Bucket{.subrequestChannel = channel,
        .bucket = kj::str(binding.getR2Bucket().getName()),
        .bindingName = kj::str(binding.getName())});
    }

    case config::Worker::Binding::R2_ADMIN: {
      uint channel = static_cast<uint>(subrequestChannels.size()) +
          IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel{binding.getR2Admin(), kj::mv(errorContext)});
      return makeGlobal(Global::R2Admin{.subrequestChannel = channel});
    }

    case config::Worker::Binding::QUEUE: {
      uint channel = static_cast<uint>(subrequestChannels.size()) +
          IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel{binding.getQueue(), kj::mv(errorContext)});

      return makeGlobal(Global::QueueBinding{.subrequestChannel = channel});
    }

    case config::Worker::Binding::WRAPPED: {
      auto wrapped = binding.getWrapped();
      kj::Vector<Global> innerGlobals;
      for (const auto& innerBinding: wrapped.getInnerBindings()) {
        KJ_IF_SOME(global,
            createBinding(workerName, conf, innerBinding, errorReporter, subrequestChannels,
                actorChannels, actorClassChannels, workerLoaderChannels, hasWorkerdDebugPortBinding,
                actorConfigs, experimental)) {
          innerGlobals.add(kj::mv(global));
        } else {
          // we've already communicated the error
          return kj::none;
        }
      }
      return makeGlobal(Global::Wrapped{
        .moduleName = kj::str(wrapped.getModuleName()),
        .entrypoint = kj::str(wrapped.getEntrypoint()),
        .innerBindings = innerGlobals.releaseAsArray(),
      });
    }

    case config::Worker::Binding::FROM_ENVIRONMENT: {
      const char* value = getenv(binding.getFromEnvironment().cStr());
      if (value == nullptr) {
        // TODO(cleanup): Maybe make a Global::Null? (Can't use nullptr_t in OneOf.) For now,
        // using JSON gets the job done hackily.
        return makeGlobal(Global::Json{kj::str("null")});
      } else {
        return makeGlobal(kj::str(value));
      }
    }

    case config::Worker::Binding::ANALYTICS_ENGINE: {
      if (!experimental) {
        errorReporter.addError(kj::str(
            "AnalyticsEngine bindings are an experimental feature which may change or go away in the future."
            "You must run workerd with `--experimental` to use this feature."));
      }

      uint channel = static_cast<uint>(subrequestChannels.size()) +
          IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(
          FutureSubrequestChannel{binding.getAnalyticsEngine(), kj::mv(errorContext)});

      return makeGlobal(Global::AnalyticsEngine{
        .subrequestChannel = channel,
        .dataset = kj::str(binding.getAnalyticsEngine().getName()),
        .version = 0,
      });
    }
    case config::Worker::Binding::HYPERDRIVE: {
      uint channel = static_cast<uint>(subrequestChannels.size()) +
          IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(
          FutureSubrequestChannel{binding.getHyperdrive().getDesignator(), kj::mv(errorContext)});
      return makeGlobal(Global::Hyperdrive{
        .subrequestChannel = channel,
        .database = kj::str(binding.getHyperdrive().getDatabase()),
        .user = kj::str(binding.getHyperdrive().getUser()),
        .password = kj::str(binding.getHyperdrive().getPassword()),
        .scheme = kj::str(binding.getHyperdrive().getScheme()),
      });
    }
    case config::Worker::Binding::UNSAFE_EVAL: {
      if (!experimental) {
        errorReporter.addError(kj::str("Unsafe eval is an experimental feature. ",
            "You must run workerd with `--experimental` to use this feature."));
        return kj::none;
      }
      return makeGlobal(Global::UnsafeEval{});
    }
    case config::Worker::Binding::MEMORY_CACHE: {
      if (!experimental) {
        errorReporter.addError(kj::str(
            "MemoryCache bindings are an experimental feature which may change or go away "
            "in the future. You must run workerd with `--experimental` to use this feature."));
        return kj::none;
      }
      auto cache = binding.getMemoryCache();
      // TODO(cleanup): Should we have some reasonable default for these so they can
      // be optional?
      if (!cache.hasLimits()) {
        errorReporter.addError(
            kj::str("MemoryCache bindings must specify limits. Please "
                    "update the binding in the worker configuration and try again."));
        return kj::none;
      }
      Global::MemoryCache cacheCopy;
      // The id is optional. If provided, then multiple bindings with the same id will
      // share the same cache. Otherwise, a unique id is generated for the cache.
      if (cache.hasId()) {
        cacheCopy.cacheId = kj::str(cache.getId());
      }
      auto limits = cache.getLimits();
      cacheCopy.maxKeys = limits.getMaxKeys();
      cacheCopy.maxValueSize = limits.getMaxValueSize();
      cacheCopy.maxTotalValueSize = limits.getMaxTotalValueSize();
      return makeGlobal(kj::mv(cacheCopy));
    }

    case config::Worker::Binding::DURABLE_OBJECT_CLASS: {
      if (!experimental) {
        errorReporter.addError(kj::str(
            "Durable Object class bindings are an experimental feature which may change or go away "
            "in the future. You must run workerd with `--experimental` to use this feature."));
        return kj::none;
      }
      uint channel = actorClassChannels.size();
      actorClassChannels.add(
          FutureActorClassChannel{binding.getDurableObjectClass(), kj::mv(errorContext)});
      return makeGlobal(Global::ActorClass{.channel = channel});
    }

    case config::Worker::Binding::WORKER_LOADER: {
      if (!experimental) {
        errorReporter.addError(kj::str(
            "Worker loader bindings are an experimental feature which may change or go away "
            "in the future. You must run workerd with `--experimental` to use this feature."));
        return kj::none;
      }

      auto loaderConf = binding.getWorkerLoader();

      FutureWorkerLoaderChannel channel;
      if (loaderConf.hasId()) {
        channel.name = kj::str(loaderConf.getId());
        channel.id = kj::str(channel.name);
      } else {
        channel.name = kj::str(bindingName);
      }

      uint channelNumber = workerLoaderChannels.size();
      workerLoaderChannels.add(kj::mv(channel));
      return makeGlobal(Global::WorkerLoader{.channel = channelNumber});
    }

    case config::Worker::Binding::WORKERD_DEBUG_PORT: {
      if (!experimental) {
        errorReporter.addError(kj::str(
            "workerdDebugPort bindings are an experimental feature which may change or go away "
            "in the future. You must run workerd with `--experimental` to use this feature."));
        return kj::none;
      }

      hasWorkerdDebugPortBinding = true;
      return makeGlobal(Global::WorkerdDebugPort{});
    }
  }
  errorReporter.addError(kj::str(errorContext,
      "has unrecognized type. Was the config compiled with a newer version of "
      "the schema?"));
}

uint startInspector(
    kj::StringPtr inspectorAddress, Server::InspectorServiceIsolateRegistrar& registrar);

void Server::abortAllActors(kj::Maybe<const kj::Exception&> reason) {
  for (auto& service: services) {
    if (WorkerService* worker = dynamic_cast<WorkerService*>(&*service.value)) {
      for (auto& [className, ns]: worker->getActorNamespaces()) {
        bool isEvictable = true;
        KJ_SWITCH_ONEOF(ns->getConfig()) {
          KJ_CASE_ONEOF(c, Durable) {
            isEvictable = c.isEvictable;
          }
          KJ_CASE_ONEOF(c, Ephemeral) {
            isEvictable = c.isEvictable;
          }
        }
        if (isEvictable) ns->abortAll(reason);
      }
    }
  }
}

// WorkerDef is an intermediate representation of everything from `config::Worker::Reader` that
// `Server::makeWorkerImpl()` needs. Similar to `WorkerSource`, we factor out this intermediate
// representation so that we can potentially build it dynamically from input that isn't a
// workerd config file.
struct Server::WorkerDef {
  CompatibilityFlags::Reader featureFlags;
  WorkerSource source;
  kj::Maybe<kj::StringPtr> moduleFallback;
  const kj::HashMap<kj::String, ActorConfig>& localActorConfigs;
  bool isDynamic;

  FutureSubrequestChannel globalOutbound;
  kj::Maybe<FutureSubrequestChannel> cacheApiOutbound;
  kj::Vector<FutureSubrequestChannel> subrequestChannels;
  kj::Vector<FutureActorChannel> actorChannels;
  kj::Vector<FutureActorClassChannel> actorClassChannels;
  kj::Vector<FutureWorkerLoaderChannel> workerLoaderChannels;
  bool hasWorkerdDebugPortBinding = false;
  kj::Array<FutureSubrequestChannel> tails;
  kj::Array<FutureSubrequestChannel> streamingTails;

  // Dynamically-loaded isolates can't directly have storage, so for now I'm using a raw capnp
  // Reader here. A default-constructed Reader will have type `none` which is appropriate for
  // dynamically-loaded workers. Same story for ContainerEngine.
  config::Worker::DurableObjectStorage::Reader actorStorageConf;
  config::Worker::ContainerEngine::Reader containerEngineConf;

  // Similar to the `compileBindings` callback passed into `Worker`'s constructor, except that
  // `ctx.exports` is taken care of separately. This is provided as a callback since `env` is
  // constructed in a vastly different way for dynamically-loaded workers.
  kj::Function<void(jsg::Lock& lock, const Worker::Api& api, v8::Local<v8::Object> target)>
      compileBindings;

  // If the WorkerDef was created from a DymamicWorkerSource and that
  // source contains a clone of the source bundle, this will take ownership.
  kj::Maybe<kj::Own<void>> maybeOwnedSourceCode;
};

class Server::WorkerLoaderNamespace: public kj::Refcounted {
 public:
  WorkerLoaderNamespace(Server& server, kj::String namespaceName)
      : server(server),
        namespaceName(kj::mv(namespaceName)) {}

  void unlink() {
    for (auto& isolate: isolates) {
      isolate.value->unlink();
    }
  }

  kj::Own<WorkerStubChannel> loadIsolate(
      kj::Maybe<kj::String> name, kj::Function<kj::Promise<DynamicWorkerSource>()> fetchSource) {
    KJ_IF_SOME(n, name) {
      return isolates
          .findOrCreate(n,
              [&]() -> decltype(isolates)::Entry {
        // This name isn't actually used in any maps nor is it ever revealed back to the app, but it
        // may be used in error logs.
        auto isolateName = kj::str(namespaceName, ':', n);

        return {.key = kj::mv(n),
          .value = kj::rc<WorkerStubImpl>(server, kj::mv(isolateName), kj::mv(fetchSource))};
      })
          .addRef()
          .toOwn();
    } else {
      auto isolateName = kj::str(namespaceName, ":dynamic:", randomUUID(server.entropySource));
      return kj::rc<WorkerStubImpl>(server, kj::mv(isolateName), kj::mv(fetchSource)).toOwn();
    }
  }

 private:
  Server& server;
  kj::String namespaceName;

  class WorkerStubImpl;
  kj::HashMap<kj::String, kj::Rc<WorkerStubImpl>> isolates;

  class NullGlobalOutboundChannel: public IoChannelFactory::SubrequestChannel {
   public:
    kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
      JSG_FAIL_REQUIRE(Error,
          "This worker is not permitted to access the internet via global functions like fetch(). "
          "It must use capabilities (such as bindings in 'env') to talk to the outside world.");
    }

    void requireAllowsTransfer() override {
      // It's difficult to get here, because the null outbound is not normally something you can
      // reference. That said, it is possible to get a `Fetcher` representing the `next` outbound
      // by pulling it off an incoming `Request` object, and in practice that points to the same
      // thing as the null outbound. You could then try to transfer it.
      //
      // We disallow this for now because it's not clear why it would be needed. That said, if it
      // is needed for some reason, it wouldn't be hard to support. But we might want to change
      // the error message it throws from startRequest(), since the error would be somewhat
      // misleading after the channel has been transferred.
      JSG_FAIL_REQUIRE(DOMDataCloneError, "The null global outbound is not transferrable.");
    }
  };

  class WorkerStubImpl final: public WorkerStubChannel, public kj::Refcounted {
   public:
    WorkerStubImpl(Server& server,
        kj::String isolateName,
        kj::Function<kj::Promise<DynamicWorkerSource>()> fetchSource)
        : startupTask(start(server, kj::mv(isolateName), kj::mv(fetchSource)).fork()) {}

    ~WorkerStubImpl() {
      unlink();
    }

    void unlink() {
      KJ_IF_SOME(s, service) {
        s->unlink();
      }
    }

    kj::Own<IoChannelFactory::SubrequestChannel> getEntrypoint(
        kj::Maybe<kj::String> name, Frankenvalue props) override {
      return kj::refcounted<SubrequestChannelImpl>(addRefToThis(), kj::mv(name), kj::mv(props));
    }

    kj::Own<IoChannelFactory::ActorClassChannel> getActorClass(
        kj::Maybe<kj::String> name, Frankenvalue props) override {
      return kj::refcounted<ActorClassImpl>(addRefToThis(), kj::mv(name), kj::mv(props));
    }

   private:
    kj::Maybe<kj::Own<WorkerService>> service;  // null if still starting up
    kj::ForkedPromise<void> startupTask;        // resolves when `service` is non-null

    kj::Promise<void> start(Server& server,
        kj::String isolateName,
        kj::Function<kj::Promise<DynamicWorkerSource>()> fetchSource) {
      auto source = co_await fetchSource();
      static const kj::HashMap<kj::String, ActorConfig> EMPTY_ACTOR_CONFIGS;

      // Rewrite the capabilities in `env` in order to build the I/O channel table.
      kj::Vector<FutureSubrequestChannel> subrequestChannels;
      kj::Vector<FutureActorClassChannel> actorClassChannels;
      source.env.rewriteCaps([&](kj::Own<Frankenvalue::CapTableEntry> entry) {
        if (auto channel = dynamic_cast<IoChannelFactory::SubrequestChannel*>(entry.get())) {
          uint channelNumber =
              subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
          subrequestChannels.add(FutureSubrequestChannel{
            .designator = kj::addRef(*channel),
            .errorContext = kj::str("Worker's env"),
          });
          return kj::heap<IoChannelCapTableEntry>(
              IoChannelCapTableEntry::SUBREQUEST, channelNumber);
        } else if (auto channel = dynamic_cast<ActorClass*>(entry.get())) {
          uint channelNumber = subrequestChannels.size();
          actorClassChannels.add(FutureActorClassChannel{
            .designator = kj::addRef(*channel),
            .errorContext = kj::str("Worker's env"),
          });
          return kj::heap<IoChannelCapTableEntry>(
              IoChannelCapTableEntry::ACTOR_CLASS, channelNumber);
        } else {
          // Generally, it shouldn't be possible to get here, but just in case, let's at least
          // provide some sort of error, although it's a vague one.
          JSG_FAIL_REQUIRE(DOMDataCloneError,
              "Dynamic 'env' contains one or more objects that are not supported for use in "
              "'env', although they would be supported in 'props'.");
        }
      });

      WorkerDef def{
        .featureFlags = source.compatibilityFlags,
        .source = kj::mv(source.source),
        .moduleFallback = kj::none,
        .localActorConfigs = EMPTY_ACTOR_CONFIGS,
        .isDynamic = true,

        // clang-format off
        .globalOutbound{
          .designator = kj::mv(source.globalOutbound)
              .orDefault([]() { return kj::refcounted<NullGlobalOutboundChannel>(); }),
          .errorContext = kj::str("Worker's globalOutbound"),
        },

        .subrequestChannels = kj::mv(subrequestChannels),
        .actorClassChannels = kj::mv(actorClassChannels),

        .tails = KJ_MAP(tail, source.tails) -> FutureSubrequestChannel {
          return {
            .designator = kj::mv(tail),
            .errorContext = kj::str("Worker's tail"),
          };
        },
        .streamingTails = KJ_MAP(tail, source.streamingTails) -> FutureSubrequestChannel {
          return {
            .designator = kj::mv(tail),
            .errorContext = kj::str("Worker's streaming tail"),
          };
        },

        .compileBindings = [env = kj::mv(source.env)](
            jsg::Lock& js, const Worker::Api& api, v8::Local<v8::Object> target) mutable {
          env.populateJsObject(js, jsg::JsObject(target));
        },

        // Note here that we always keep the ownContent from the source, even if
        // ownContentIsRpcResponse is true. This is safe in workerd because we
        // are single-threaded here and we don't need to worry about the cross-thread
        // ownership issues. For the downstream use, however, we need to be careful
        // to not copy the ownContent if it is an RPC response.
        .maybeOwnedSourceCode = kj::mv(source.ownContent),
        // clang-format on
      };

      DynamicErrorReporter errorReporter;

      auto service = co_await server.makeWorkerImpl(isolateName, kj::mv(def), {}, errorReporter);
      errorReporter.throwIfErrors();

      service->link(errorReporter);
      errorReporter.throwIfErrors();

      this->service = kj::mv(service);
    }

    class SubrequestChannelImpl final: public IoChannelFactory::SubrequestChannel {
     public:
      SubrequestChannelImpl(
          kj::Rc<WorkerStubImpl> isolate, kj::Maybe<kj::String> entrypointName, Frankenvalue props)
          : isolate(kj::mv(isolate)),
            entrypointName(kj::mv(entrypointName)),
            props(kj::mv(props)) {}

      kj::Own<WorkerInterface> startRequest(
          IoChannelFactory::SubrequestMetadata metadata) override {
        if (isolate->service == kj::none) {
          return newPromisedWorkerInterface(
              isolate->startupTask.addBranch().then([this, metadata = kj::mv(metadata)]() mutable {
            return startRequestImpl(kj::mv(metadata));
          }));
        } else {
          return startRequestImpl(kj::mv(metadata));
        }
      }

      void requireAllowsTransfer() override {
        throwDynamicEntrypointTransferError();
      }

     private:
      kj::Rc<WorkerStubImpl> isolate;
      kj::Maybe<kj::String> entrypointName;
      Frankenvalue props;  // moved away when `entrypointService` is initialized

      kj::Maybe<kj::Own<Service>> entrypointService;

      kj::Own<WorkerInterface> startRequestImpl(IoChannelFactory::SubrequestMetadata metadata) {
        auto& service = KJ_ASSERT_NONNULL(isolate->service);
        if (entrypointService == kj::none) {
          entrypointService = service->getEntrypoint(entrypointName, kj::mv(props));
        }
        KJ_IF_SOME(ep, entrypointService) {
          return ep->startRequest(kj::mv(metadata));
        } else {
          KJ_IF_SOME(en, entrypointName) {
            JSG_FAIL_REQUIRE(Error, "Worker has no such entrypoint: ", en);
          } else {
            JSG_FAIL_REQUIRE(Error, "Worker has no default entrypoint.");
          }
        }
      }
    };

    class ActorClassImpl final: public ActorClass {
     public:
      ActorClassImpl(
          kj::Rc<WorkerStubImpl> isolate, kj::Maybe<kj::String> entrypointName, Frankenvalue props)
          : isolate(kj::mv(isolate)),
            entrypointName(kj::mv(entrypointName)),
            props(kj::mv(props)) {}

      void requireAllowsTransfer() override {
        throwDynamicEntrypointTransferError();
      }

      kj::Maybe<kj::Promise<void>> whenReady() override {
        if (inner != kj::none) return kj::none;

        KJ_IF_SOME(service, isolate->service) {
          inner = service->getActorClass(entrypointName, kj::mv(props));
          return kj::none;
        }

        // Have to wait for the isolate to start up.
        return isolate->startupTask.addBranch().then([this]() {
          if (inner == kj::none) {
            inner =
                KJ_ASSERT_NONNULL(isolate->service)->getActorClass(entrypointName, kj::mv(props));
          }
        });
      }

      kj::Own<Worker::Actor> newActor(kj::Maybe<RequestTracker&> tracker,
          Worker::Actor::Id actorId,
          Worker::Actor::MakeActorCacheFunc makeActorCache,
          Worker::Actor::MakeStorageFunc makeStorage,
          kj::Own<Worker::Actor::Loopback> loopback,
          kj::Maybe<kj::Own<Worker::Actor::HibernationManager>> manager,
          kj::Maybe<rpc::Container::Client> container,
          kj::Maybe<Worker::Actor::FacetManager&> facetManager) override {
        return getInner().newActor(tracker, kj::mv(actorId), kj::mv(makeActorCache),
            kj::mv(makeStorage), kj::mv(loopback), kj::mv(manager), kj::mv(container),
            facetManager);
      }

      kj::Own<WorkerInterface> startRequest(
          IoChannelFactory::SubrequestMetadata metadata, kj::Own<Worker::Actor> actor) override {
        return getInner().startRequest(kj::mv(metadata), kj::mv(actor));
      }

     private:
      kj::Rc<WorkerStubImpl> isolate;
      kj::Maybe<kj::String> entrypointName;
      Frankenvalue props;  // moved away when `inner` is initialized

      kj::Maybe<kj::Own<ActorClass>> inner;

      ActorClass& getInner() {
        return *KJ_ASSERT_NONNULL(
            inner, "ActorClassChannel is not ready yet; should have awaited whenReady()");
      }
    };
  };
};

void Server::unlinkWorkerLoaders() {
  for (auto& loader: workerLoaderNamespaces) {
    loader.value->unlink();
  }
  for (auto& loader: anonymousWorkerLoaderNamespaces) {
    loader->unlink();
  }
}

kj::Own<WorkerStubChannel> Server::WorkerService::loadIsolate(uint loaderChannel,
    kj::Maybe<kj::String> name,
    kj::Function<kj::Promise<DynamicWorkerSource>()> fetchSource) {
  auto& channels =
      KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(), "link() has not been called");
  KJ_REQUIRE(loaderChannel < channels.workerLoaders.size(), "invalid worker loader channel number");

  return channels.workerLoaders[loaderChannel]->loadIsolate(kj::mv(name), kj::mv(fetchSource));
}

kj::Promise<kj::Own<Server::Service>> Server::makeWorker(kj::StringPtr name,
    config::Worker::Reader conf,
    capnp::List<config::Extension>::Reader extensions) {
  TRACE_EVENT("workerd", "Server::makeWorker()", "name", name.cStr());
  auto& localActorConfigs = KJ_ASSERT_NONNULL(actorConfigs.find(name));

  ConfigErrorReporter errorReporter(*this, name);

  capnp::MallocMessageBuilder arena;
  // TODO(beta): Factor out FeatureFlags from WorkerBundle.
  auto featureFlags = arena.initRoot<CompatibilityFlags>();

  KJ_IF_SOME(overrideDate, testCompatibilityDateOverride) {
    // When testCompatibilityDateOverride is set, the config must NOT specify compatibilityDate.
    if (conf.hasCompatibilityDate()) {
      errorReporter.addError(kj::str(
          "Worker specifies compatibilityDate but --compat-date was provided. "
          "When using --compat-date, workers must not specify compatibilityDate in the config. "
          "Use compatibilityFlags to enable/disable specific flags if needed."));
    }
    // Use FUTURE_FOR_TEST to allow any valid date (including far future like 2999-12-31)
    // without validation against CODE_VERSION or current date.
    compileCompatibilityFlags(overrideDate, conf.getCompatibilityFlags(), featureFlags,
        errorReporter, experimental, CompatibilityDateValidation::FUTURE_FOR_TEST);
  } else if (conf.hasCompatibilityDate()) {
    compileCompatibilityFlags(conf.getCompatibilityDate(), conf.getCompatibilityFlags(),
        featureFlags, errorReporter, experimental, CompatibilityDateValidation::CODE_VERSION);
  } else {
    errorReporter.addError(kj::str("Worker must specify compatibilityDate."));
  }

  kj::Vector<FutureSubrequestChannel> subrequestChannels;
  kj::Vector<FutureActorChannel> actorChannels;
  kj::Vector<FutureActorClassChannel> actorClassChannels;
  kj::Vector<FutureWorkerLoaderChannel> workerLoaderChannels;
  bool hasWorkerdDebugPortBinding = false;

  auto confBindings = conf.getBindings();
  kj::Vector<WorkerdApi::Global> globals(confBindings.size());
  for (auto binding: confBindings) {
    KJ_IF_SOME(global,
        createBinding(name, conf, binding, errorReporter, subrequestChannels, actorChannels,
            actorClassChannels, workerLoaderChannels, hasWorkerdDebugPortBinding, actorConfigs,
            experimental)) {
      globals.add(kj::mv(global));
    }
  }

  // Construct `WorkerDef` from `conf`.
  WorkerDef def{
    .featureFlags = featureFlags.asReader(),
    .source = WorkerdApi::extractSource(name, conf, featureFlags.asReader(), errorReporter),
    .moduleFallback = conf.hasModuleFallback() ? kj::some(conf.getModuleFallback()) : kj::none,
    .localActorConfigs = localActorConfigs,
    .isDynamic = false,

    .globalOutbound{
      .designator = conf.getGlobalOutbound(),
      .errorContext = kj::str("Worker \"", name, "\"'s globalOutbound"),
    },

    .cacheApiOutbound = conf.hasCacheApiOutbound()
        ? kj::some(FutureSubrequestChannel{
            .designator = conf.getCacheApiOutbound(),
            .errorContext = kj::str("Worker \"", name, "\"'s cacheApiOutbound"),
          })
        : kj::none,

    .subrequestChannels = kj::mv(subrequestChannels),
    .actorChannels = kj::mv(actorChannels),
    .actorClassChannels = kj::mv(actorClassChannels),
    .workerLoaderChannels = kj::mv(workerLoaderChannels),
    .hasWorkerdDebugPortBinding = hasWorkerdDebugPortBinding,

    // clang-format off
    .tails = KJ_MAP(tail, conf.getTails()) -> FutureSubrequestChannel {
      return {
        .designator = tail,
        .errorContext = kj::str("Worker \"", name, "\"'s tails"),
      };
    },

    .streamingTails = KJ_MAP(streamingTail, conf.getStreamingTails()) -> FutureSubrequestChannel {
      return {
        .designator = streamingTail,
        .errorContext = kj::str("Worker \"", name, "\"'s streaming tails"),
      };
    },

    .actorStorageConf = conf.getDurableObjectStorage(),
    .containerEngineConf = conf.getContainerEngine(),

    .compileBindings = [globals = kj::mv(globals)](
        jsg::Lock& lock, const Worker::Api& api, v8::Local<v8::Object> target) {
      return WorkerdApi::from(api).compileGlobals(lock, globals, target, 1);
    },
    // clang-format on
  };

  co_return co_await makeWorkerImpl(name, kj::mv(def), extensions, errorReporter);
}

kj::Promise<kj::Own<Server::WorkerService>> Server::makeWorkerImpl(kj::StringPtr name,
    WorkerDef def,
    capnp::List<config::Extension>::Reader extensions,
    ErrorReporter& errorReporter) {
  // Load Python artifacts if this is a Python worker
  co_await preloadPython(name, def, errorReporter);

  auto jsgobserver = kj::atomicRefcounted<JsgIsolateObserver>();
  auto observer = kj::atomicRefcounted<IsolateObserver>();
  auto limitEnforcer = kj::refcounted<NullIsolateLimitEnforcer>();

  // Create the FsMap that will be used to map known file system
  // roots to configurable locations.
  // TODO(node-fs): This is set up to allow users to configure the "mount"
  // points for known roots but we currently do not expose that in the
  // config. So for now this just uses the defaults.
  auto workerFs = newWorkerFileSystem(kj::heap<FsMap>(), getBundleDirectory(def.source));

  // TODO(soon): Either make python workers support the new module registry before
  // NMR is defaulted on, or disable NMR by default when python workers are enabled.
  // While NMR is experimental, we'll just throw an error if both are enabled.
  if (def.featureFlags.getPythonWorkers()) {
    KJ_REQUIRE(!def.featureFlags.getNewModuleRegistry(),
        "Python workers do not currently support the new ModuleRegistry implementation. "
        "Please disable the new ModuleRegistry feature flag to use Python workers.");
  }

  bool usingNewModuleRegistry = def.featureFlags.getNewModuleRegistry();
  kj::Maybe<kj::Arc<jsg::modules::ModuleRegistry>> newModuleRegistry;
  // TODO(soon): Python workers do not currently support the new module registry.
  if (usingNewModuleRegistry) {
    KJ_REQUIRE(experimental,
        "The new ModuleRegistry implementation is an experimental feature. "
        "You must run workerd with `--experimental` to use this feature.");

    // We use the same path for modules that the virtual file system uses.
    // For instance, if the user specifies a bundle path of "/foo/bar" and
    // there is a module in the bundle at "/foo/bar/baz.js", then the module's
    // import specifier url will be "file:///foo/bar/baz.js".
    const jsg::Url& bundleBase = workerFs->getBundleRoot();

    // In workerd the module registry is always associated with just a single
    // worker instance, so we initialize it here. In production, however, a
    // single instance may be shared across multiple replicas.
    kj::Maybe<kj::String> maybeFallbackService;
    KJ_IF_SOME(moduleFallback, def.moduleFallback) {
      maybeFallbackService = kj::str(moduleFallback);
    }

    using ArtifactBundler = workerd::api::pyodide::ArtifactBundler;
    auto isPythonWorker = def.featureFlags.getPythonWorkers();
    auto artifactBundler = isPythonWorker
        ? ArtifactBundler::makePackagesOnlyBundler(pythonConfig.pyodidePackageManager)
        : ArtifactBundler::makeDisabledBundler();

    newModuleRegistry = WorkerdApi::newWorkerdModuleRegistry(*jsgobserver,
        def.source.variant.tryGet<Worker::Script::ModulesSource>(), def.featureFlags, pythonConfig,
        bundleBase, extensions, kj::mv(maybeFallbackService), kj::mv(artifactBundler));
  }

  auto isolateGroup = v8::IsolateGroup::GetDefault();
  auto api = kj::heap<WorkerdApi>(globalContext->v8System, def.featureFlags, extensions,
      limitEnforcer->getCreateParams(), isolateGroup, kj::mv(jsgobserver), *memoryCacheProvider,
      pythonConfig);

  auto inspectorPolicy = Worker::Isolate::InspectorPolicy::DISALLOW;
  if (inspectorOverride != kj::none) {
    // For workerd, if the inspector is enabled, it is always fully trusted.
    inspectorPolicy = Worker::Isolate::InspectorPolicy::ALLOW_FULLY_TRUSTED;
  }
  Worker::LoggingOptions isolateLoggingOptions = loggingOptions;
  isolateLoggingOptions.consoleMode =
      def.source.variant.is<WorkerSource::ScriptSource>() && !usingNewModuleRegistry
      ? Worker::ConsoleMode::INSPECTOR_ONLY
      : loggingOptions.consoleMode;
  auto isolate = kj::atomicRefcounted<Worker::Isolate>(kj::mv(api), kj::mv(observer), name,
      kj::mv(limitEnforcer), inspectorPolicy, kj::mv(isolateLoggingOptions));

  // If we are using the inspector, we need to register the Worker::Isolate
  // with the inspector service.
  KJ_IF_SOME(isolateRegistrar, inspectorIsolateRegistrar) {
    isolateRegistrar->registerIsolate(name, isolate.get());
  }

  if (!usingNewModuleRegistry) {
    KJ_IF_SOME(moduleFallback, def.moduleFallback) {
      KJ_REQUIRE(experimental,
          "The module fallback service is an experimental feature. "
          "You must run workerd with `--experimental` to use the module fallback service.");
      // If the config has the moduleFallback option, then we are going to set up the ability
      // to load certain modules from a fallback service. This is generally intended for local
      // dev/testing purposes only.
      auto& apiIsolate = isolate->getApi();
      auto fallbackClient =
          kj::heap<workerd::fallback::FallbackServiceClient>(kj::str(moduleFallback));
      apiIsolate.setModuleFallbackCallback(
          [client = kj::mv(fallbackClient), featureFlags = apiIsolate.getFeatureFlags()](
              jsg::Lock& js, kj::StringPtr specifier, kj::Maybe<kj::String> referrer,
              jsg::CompilationObserver& observer, jsg::ModuleRegistry::ResolveMethod method,
              kj::Maybe<kj::StringPtr> rawSpecifier) mutable
          -> kj::Maybe<kj::OneOf<kj::String, jsg::ModuleRegistry::ModuleInfo>> {
        kj::HashMap<kj::StringPtr, kj::StringPtr> attributes;
        KJ_IF_SOME(moduleOrRedirect,
            client->tryResolve(workerd::fallback::Version::V1,
                method == jsg::ModuleRegistry::ResolveMethod::IMPORT
                    ? workerd::fallback::ImportType::IMPORT
                    : workerd::fallback::ImportType::REQUIRE,
                specifier, rawSpecifier.orDefault(nullptr), referrer.orDefault(kj::String()),
                attributes)) {
          KJ_SWITCH_ONEOF(moduleOrRedirect) {
            KJ_CASE_ONEOF(redirect, kj::String) {
              // If a string is returned, then the fallback service returned a 301 redirect.
              // The value is the specifier of the new target module.
              return kj::Maybe(kj::mv(redirect));
            }
            KJ_CASE_ONEOF(module, kj::Own<config::Worker::Module::Reader>) {
              KJ_IF_SOME(module,
                  WorkerdApi::tryCompileModule(js, *module, observer, featureFlags)) {
                return kj::Maybe(kj::mv(module));
              }
              KJ_LOG(ERROR, "Fallback service does not support this module type", module->which());
            }
          }
        }

        return kj::none;
      });
    }
  }

  using ArtifactBundler = workerd::api::pyodide::ArtifactBundler;
  auto isPythonWorker = def.featureFlags.getPythonWorkers();
  auto artifactBundler = isPythonWorker
      ? ArtifactBundler::makePackagesOnlyBundler(pythonConfig.pyodidePackageManager)
      : ArtifactBundler::makeDisabledBundler();

  auto script = isolate->newScript(name, def.source, IsolateObserver::StartType::COLD,
      SpanParent(nullptr), workerFs.attach(kj::mv(def.maybeOwnedSourceCode)), false, errorReporter,
      kj::mv(artifactBundler), kj::mv(newModuleRegistry));

  using Global = WorkerdApi::Global;
  jsg::V8Ref<v8::Object> ctxExportsHandle = nullptr;
  auto compileBindings = [&](jsg::Lock& lock, const Worker::Api& api, v8::Local<v8::Object> target,
                             v8::Local<v8::Object> ctxExports) {
    // We can't fill in ctx.exports yet because we need to run the validator first to discover
    // entrypoints, which we cannot do until after the Worker constructor completes. We are
    // permitted to hold a handle until then, though.
    ctxExportsHandle = lock.v8Ref(ctxExports);

    return def.compileBindings(lock, api, target);
  };
  auto worker = kj::atomicRefcounted<Worker>(kj::mv(script), kj::atomicRefcounted<WorkerObserver>(),
      kj::mv(compileBindings), IsolateObserver::StartType::COLD, SpanParent(nullptr),
      Worker::Lock::TakeSynchronously(kj::none), errorReporter);

  uint totalActorChannels = 0;

  worker->runInLockScope(Worker::Lock::TakeSynchronously(kj::none), [&](Worker::Lock& lock) {
    lock.validateHandlers(errorReporter);

    // Build `ctx.exports` based on the entrypoints reported by `validateHandlers()`.
    kj::Vector<Global> ctxExports(
        errorReporter.namedEntrypoints.size() + def.localActorConfigs.size());

    // Start numbering loopback channels for stateless entrypoints after the last subrequest
    // channel used by bindings.
    uint nextSubrequestChannel =
        def.subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
    if (errorReporter.defaultEntrypoint != kj::none) {
      ctxExports.add(Global{.name = kj::str("default"),
        .value = Global::LoopbackServiceStub{.channel = nextSubrequestChannel++}});
    }
    for (auto& ep: errorReporter.namedEntrypoints) {
      // Workflow classes are treated as stateless entrypoints for runtime purposes, but should
      // NOT be reflected in ctx.exports.
      // TODO(someday): Currently Workflows must be given a name independent of their class name,
      //   and the binding must reference that name. If the name were just the class name -- like
      //   Durable Object namespaces -- then we could put a `Workflow` binding into `ctx.exports`.
      if (!errorReporter.workflowClasses.contains(ep.key)) {
        ctxExports.add(Global{.name = kj::str(ep.key),
          .value = Global::LoopbackServiceStub{.channel = nextSubrequestChannel++}});
      }
    }

    // Start numbering loopback channels for actor classes after the last actor channel and actor
    // class channel used by bindings. Note that every exported actor class will have a ctx.exports
    // entry, but only the ones that have storage configured will be namespace bindings; the others
    // will be simply actor class bindings, which can be used with facets. We will iterate over
    // the exported class names and cross-reference with the storage config. Note that if the
    // storage config contains a class name that isn't among the exports, we won't create a
    // ctx.exports entry for it (it wouldn't work anyway).
    uint nextActorChannel = def.actorChannels.size();
    uint nextActorClassChannel = def.actorClassChannels.size();
    for (auto& className: errorReporter.actorClasses) {
      uint actorClassChannel = nextActorClassChannel++;

      decltype(Global::value) value;
      KJ_IF_SOME(ns, def.localActorConfigs.find(className)) {
        // This class has storage attached. We'll create a loopback actor namespace binding.
        KJ_SWITCH_ONEOF(ns) {
          KJ_CASE_ONEOF(durable, Durable) {
            value = Global::LoopbackDurableActorNamespace{
              .actorChannel = nextActorChannel++,
              .uniqueKey = durable.uniqueKey,
              .classChannel = actorClassChannel,
            };
          }
          KJ_CASE_ONEOF(ephemeral, Ephemeral) {
            value = Global::LoopbackEphemeralActorNamespace{
              .actorChannel = nextActorChannel++,
              .classChannel = actorClassChannel,
            };
          }
        }
      } else {
        // No storage attached. We'll create an actual class binding (for use with facets).
        value = Global::LoopbackActorClass{.channel = actorClassChannel};
      }
      ctxExports.add(Global{.name = kj::str(className), .value = kj::mv(value)});
    }
    totalActorChannels = nextActorChannel;

    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
      WorkerdApi::from(worker->getIsolate().getApi())
          .compileGlobals(lock, ctxExports, ctxExportsHandle.getHandle(js), 1);
    });

    // As an optimization, drop this now while we have the lock.
    { auto drop = kj::mv(ctxExportsHandle); }
  });

  auto linkCallback = [this, def = kj::mv(def), totalActorChannels](WorkerService& workerService,
                          Worker::ValidationErrorReporter& errorReporter) mutable {
    WorkerService::LinkedIoChannels result{.alarmScheduler = *alarmScheduler};

    auto entrypointNames = workerService.getEntrypointNames();
    auto actorClassNames = workerService.getActorClassNames();

    auto services = kj::heapArrayBuilder<kj::Own<IoChannelFactory::SubrequestChannel>>(
        def.subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT +
        entrypointNames.size() + workerService.hasDefaultEntrypoint());

    auto globalService = kj::mv(def.globalOutbound).lookup(*this);

    // Bind both "next" and "null" to the global outbound. (The difference between these is a
    // legacy artifact that no one should be depending on.)
    static_assert(IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT == 2);
    services.add(kj::addRef(*globalService));
    services.add(kj::mv(globalService));

    for (auto& channel: def.subrequestChannels) {
      services.add(kj::mv(channel).lookup(*this));
    }

    // Link the ctx.exports self-referential channels. Note that it's important these are added
    // in exactyl the same order as the channels were allocated earlier when we compiled the
    // ctx.exports bindings.
    if (workerService.hasDefaultEntrypoint()) {
      services.add(workerService.getLoopbackEntrypoint(/*name=*/kj::none));
    }
    for (auto& ep: entrypointNames) {
      services.add(workerService.getLoopbackEntrypoint(ep));
    }

    result.subrequest = services.finish();

    // Set up actor class channels
    auto actorClasses = kj::heapArrayBuilder<kj::Own<ActorClass>>(
        def.actorClassChannels.size() + actorClassNames.size());

    for (auto& channel: def.actorClassChannels) {
      actorClasses.add(kj::mv(channel).lookup(*this));
    }

    auto linkedActorChannels =
        kj::heapArrayBuilder<kj::Maybe<WorkerService::ActorNamespace&>>(totalActorChannels);

    for (auto& channel: def.actorChannels) {
      WorkerService* targetService = &workerService;
      if (channel.designator.hasServiceName()) {
        auto& svc = KJ_UNWRAP_OR(this->services.find(channel.designator.getServiceName()), {
          // error was reported earlier
          linkedActorChannels.add(kj::none);
          continue;
        });
        targetService = dynamic_cast<WorkerService*>(svc.get());
        if (targetService == nullptr) {
          // error was reported earlier
          linkedActorChannels.add(kj::none);
          continue;
        }
      }

      // (If getActorNamespace() returns null, an error was reported earlier.)
      linkedActorChannels.add(targetService->getActorNamespace(channel.designator.getClassName()));
    };

    // Link the ctx.exports self-referential actor channels. Again, it's important that these
    // be added in the same order as before. kj::HashMap iteration order is deterministic, and
    // is exactly insertion order as long as no entries have been removed, so we can expect that
    // `workerService.getActorClassNames()` iterates in the same order as
    // `errorReporter.actorClasses` did earlier. As before, every exported class gets an actor
    // class channel, but only the ones with configured storage will also get namespace channels.
    auto& selfActorNamespaces = workerService.getActorNamespaces();
    for (auto& className: actorClassNames) {
      actorClasses.add(workerService.getLoopbackActorClass(className));
      KJ_IF_SOME(ns, selfActorNamespaces.find(className)) {
        linkedActorChannels.add(*ns);
      }
    }

    result.actor = linkedActorChannels.finish();
    result.actorClass = actorClasses.finish();

    KJ_IF_SOME(out, def.cacheApiOutbound) {
      result.cache = kj::mv(out).lookup(*this);
    }

    if (def.actorStorageConf.isLocalDisk()) {
      kj::StringPtr diskName = def.actorStorageConf.getLocalDisk();
      KJ_IF_SOME(svc, this->services.find(def.actorStorageConf.getLocalDisk())) {
        auto diskSvc = dynamic_cast<DiskDirectoryService*>(svc.get());
        if (diskSvc == nullptr) {
          errorReporter.addError(kj::str("durableObjectStorage config refers to the service \"",
              diskName, "\", but that service is not a local disk service."));
        } else KJ_IF_SOME(dir, diskSvc->getWritable()) {
          result.actorStorage = dir;
        } else {
          errorReporter.addError(
              kj::str("durableObjectStorage config refers to the disk service \"", diskName,
                  "\", but that service is defined read-only."));
        }
      } else {
        errorReporter.addError(kj::str("durableObjectStorage config refers to a service \"",
            diskName, "\", but no such service is defined."));
      }
    }

    kj::HashMap<kj::StringPtr, WorkerService::ActorNamespace&> durableNamespacesByUniqueKey;
    for (auto& [className, ns]: workerService.getActorNamespaces()) {
      KJ_IF_SOME(config, ns->getConfig().tryGet<Server::Durable>()) {
        auto& actorNs =
            ns;  // clangd gets confused trying to use ns directly in the capture below??

        auto idFactory = kj::heap<ActorIdFactoryImpl>(config.uniqueKey);

        alarmScheduler->registerNamespace(config.uniqueKey,
            [&actorNs, idFactory = kj::mv(idFactory)](
                kj::String idStr) mutable -> kj::Own<WorkerInterface> {
          Worker::Actor::Id id = idFactory->idFromString(kj::mv(idStr));
          auto actorContainer = actorNs->getActorContainer(kj::mv(id));
          return newPromisedWorkerInterface(actorContainer->startRequest({}));
        });
      }
    }

    result.tails = KJ_MAP(tail, def.tails) { return kj::mv(tail).lookup(*this); };

    result.streamingTails = KJ_MAP(tail, def.streamingTails) { return kj::mv(tail).lookup(*this); };

    result.workerLoaders = KJ_MAP(il, def.workerLoaderChannels) {
      KJ_IF_SOME(id, il.id) {
        return workerLoaderNamespaces
            .findOrCreate(id, [&]() -> decltype(workerLoaderNamespaces)::Entry {
          return {
            .key = kj::mv(id),
            .value = kj::rc<WorkerLoaderNamespace>(*this, kj::mv(il.name)),
          };
        }).addRef();
      } else {
        return anonymousWorkerLoaderNamespaces
            .add(kj::rc<WorkerLoaderNamespace>(*this, kj::mv(il.name)))
            .addRef();
      }
    };

    if (def.hasWorkerdDebugPortBinding) {
      result.workerdDebugPortNetwork = network;
    }

    return result;
  };

  kj::Maybe<kj::String> dockerPath = kj::none;
  kj::Maybe<kj::String> containerEgressInterceptorImage = kj::none;
  switch (def.containerEngineConf.which()) {
    case config::Worker::ContainerEngine::NONE:
      // No container engine configured
      break;
    case config::Worker::ContainerEngine::LOCAL_DOCKER: {
      auto dockerConf = def.containerEngineConf.getLocalDocker();
      dockerPath = kj::str(dockerConf.getSocketPath());
      if (dockerConf.hasContainerEgressInterceptorImage()) {
        containerEgressInterceptorImage = kj::str(dockerConf.getContainerEgressInterceptorImage());
      }
      break;
    }
  }

  kj::Maybe<kj::StringPtr> serviceName;
  if (!def.isDynamic) serviceName = name;

  auto result =
      kj::refcounted<WorkerService>(channelTokenHandler, serviceName, globalContext->threadContext,
          monotonicClock, kj::mv(worker), kj::mv(errorReporter.defaultEntrypoint),
          kj::mv(errorReporter.namedEntrypoints), kj::mv(errorReporter.actorClasses),
          kj::mv(linkCallback), KJ_BIND_METHOD(*this, abortAllActors), kj::mv(dockerPath),
          kj::mv(containerEgressInterceptorImage), def.isDynamic);
  result->initActorNamespaces(def.localActorConfigs, network);
  co_return result;
}

// =======================================================================================

kj::Promise<kj::Own<Server::Service>> Server::makeService(config::Service::Reader conf,
    kj::HttpHeaderTable::Builder& headerTableBuilder,
    capnp::List<config::Extension>::Reader extensions) {
  kj::StringPtr name = conf.getName();

  switch (conf.which()) {
    case config::Service::UNSPECIFIED:
      reportConfigError(kj::str("Service named \"", name, "\" does not specify what to serve."));
      co_return makeInvalidConfigService();

    case config::Service::EXTERNAL:
      co_return makeExternalService(name, conf.getExternal(), headerTableBuilder);

    case config::Service::NETWORK:
      co_return makeNetworkService(conf.getNetwork());

    case config::Service::WORKER:
      co_return co_await makeWorker(name, conf.getWorker(), extensions);

    case config::Service::DISK:
      co_return makeDiskDirectoryService(name, conf.getDisk(), headerTableBuilder);
  }

  reportConfigError(kj::str("Service named \"", name,
      "\" has unrecognized type. Was the config compiled with a "
      "newer version of the schema?"));
  co_return makeInvalidConfigService();
}

void Server::taskFailed(kj::Exception&& exception) {
  fatalFulfiller->reject(kj::mv(exception));
}

kj::Own<Server::Service> Server::lookupService(
    config::ServiceDesignator::Reader designator, kj::String errorContext) {
  kj::StringPtr targetName = designator.getName();
  Service* service = KJ_UNWRAP_OR(services.find(targetName), {
    reportConfigError(kj::str(errorContext, " refers to a service \"", targetName,
        "\", but no such service is defined."));
    return kj::addRef(*invalidConfigServiceSingleton);
  });

  kj::Maybe<kj::StringPtr> entrypointName;
  if (designator.hasEntrypoint()) {
    entrypointName = designator.getEntrypoint();
  }

  auto props = [&]() -> Frankenvalue {
    auto props = designator.getProps();
    switch (props.which()) {
      case config::ServiceDesignator::Props::EMPTY:
        return {};
      case config::ServiceDesignator::Props::JSON:
        return Frankenvalue::fromJson(kj::str(props.getJson()));
    }
    reportConfigError(kj::str(errorContext,
        " has unrecognized props type. Was the config compiled with a "
        "newer version of the schema?"));
    return {};
  }();

  if (WorkerService* worker = dynamic_cast<WorkerService*>(service)) {
    KJ_IF_SOME(ep, worker->getEntrypoint(entrypointName, kj::mv(props))) {
      return kj::mv(ep);
    } else KJ_IF_SOME(ep, entrypointName) {
      reportConfigError(kj::str(errorContext, " refers to service \"", targetName,
          "\" with a named entrypoint \"", ep, "\", but \"", targetName,
          "\" has no such named entrypoint."));
      return kj::addRef(*invalidConfigServiceSingleton);
    } else {
      reportConfigError(kj::str(errorContext, " refers to service \"", targetName,
          "\", but does not specify an entrypoint, and the service does not have a "
          "default entrypoint."));
      return kj::addRef(*invalidConfigServiceSingleton);
    }
  } else {
    KJ_IF_SOME(ep, entrypointName) {
      reportConfigError(kj::str(errorContext, " refers to service \"", targetName,
          "\" with a named entrypoint \"", ep, "\", but \"", targetName,
          "\" is not a Worker, so does not have any named entrypoints."));
    } else if (!props.empty()) {
      reportConfigError(kj::str(errorContext, " refers to service \"", targetName,
          "\" and provides a `props` value, but \"", targetName,
          "\" is not a Worker, so cannot accept `props`"));
    }

    return kj::addRef(*service);
  }
}

kj::Own<Server::ActorClass> Server::lookupActorClass(
    config::ServiceDesignator::Reader designator, kj::String errorContext) {
  // TODO(cleanup): There's a lot of repeated code with lookupService(), should it be refactored?

  kj::StringPtr targetName = designator.getName();
  Service* service = KJ_UNWRAP_OR(services.find(targetName), {
    reportConfigError(kj::str(errorContext, " refers to a service \"", targetName,
        "\", but no such service is defined."));
    return kj::addRef(*invalidConfigActorClassSingleton);
  });

  kj::Maybe<kj::StringPtr> entrypointName;
  if (designator.hasEntrypoint()) {
    entrypointName = designator.getEntrypoint();
  }

  auto props = [&]() -> Frankenvalue {
    auto props = designator.getProps();
    switch (props.which()) {
      case config::ServiceDesignator::Props::EMPTY:
        return {};
      case config::ServiceDesignator::Props::JSON:
        return Frankenvalue::fromJson(kj::str(props.getJson()));
    }
    reportConfigError(kj::str(errorContext,
        " has unrecognized props type. Was the config compiled with a "
        "newer version of the schema?"));
    return {};
  }();

  if (WorkerService* worker = dynamic_cast<WorkerService*>(service)) {
    KJ_IF_SOME(ep, worker->getActorClass(entrypointName, kj::mv(props))) {
      return kj::mv(ep);
    } else KJ_IF_SOME(ep, entrypointName) {
      reportConfigError(kj::str(errorContext, " refers to service \"", targetName,
          "\" with a Durable Object entrypoint \"", ep, "\", but \"", targetName,
          "\" has no such exported entrypoint class."));
      return kj::addRef(*invalidConfigActorClassSingleton);
    } else {
      reportConfigError(kj::str(errorContext, " refers to service \"", targetName,
          "\", but does not specify an entrypoint, and the service does export a "
          "Durable Object class as its default entrypoint."));
      return kj::addRef(*invalidConfigActorClassSingleton);
    }
  } else {
    KJ_IF_SOME(ep, entrypointName) {
      reportConfigError(kj::str(errorContext, " refers to service \"", targetName,
          "\" with a named Durable Object entrypoint \"", ep, "\", but \"", targetName,
          "\" is not a Worker, so does not have any named entrypoints."));
    } else {
      reportConfigError(kj::str(errorContext, " refers to service \"", targetName,
          "\" as a Durable Object class, but \"", targetName,
          "\" is not a Worker, so cannot be used as a class."));
    }

    return kj::addRef(*invalidConfigActorClassSingleton);
  }
}

kj::Own<IoChannelFactory::SubrequestChannel> Server::resolveEntrypoint(
    kj::StringPtr serviceName, kj::Maybe<kj::StringPtr> entrypoint, Frankenvalue props) {
  auto& service = *JSG_REQUIRE_NONNULL(services.find(serviceName), Error,
      "Stub refers to a service that doesn't exist: ", serviceName);

  auto& worker = JSG_REQUIRE_NONNULL(kj::tryDowncast<WorkerService>(service), Error,
      "Stub refers to a service that is not a Worker: ", serviceName);

  return JSG_REQUIRE_NONNULL(worker.getEntrypoint(entrypoint, kj::mv(props)), Error,
      "Stub refers to a an entrypoint of the target service that doesn't exist: ",
      entrypoint.orDefault("default"));
}

kj::Own<IoChannelFactory::ActorClassChannel> Server::resolveActorClass(
    kj::StringPtr serviceName, kj::Maybe<kj::StringPtr> entrypoint, Frankenvalue props) {
  auto& service = *JSG_REQUIRE_NONNULL(services.find(serviceName), Error,
      "Stub refers to a service that doesn't exist: ", serviceName);

  auto& worker = JSG_REQUIRE_NONNULL(kj::tryDowncast<WorkerService>(service), Error,
      "Stub refers to a service that is not a Worker: ", serviceName);

  return JSG_REQUIRE_NONNULL(worker.getActorClass(entrypoint, kj::mv(props)), Error,
      "Stub refers to a an entrypoint of the target service that doesn't exist: ",
      entrypoint.orDefault("default"));
}

// =======================================================================================

class Server::WorkerdBootstrapImpl final: public rpc::WorkerdBootstrap::Server {
 public:
  WorkerdBootstrapImpl(kj::Own<IoChannelFactory::SubrequestChannel> service,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
      : service(kj::mv(service)),
        httpOverCapnpFactory(httpOverCapnpFactory) {}

  kj::Promise<void> startEvent(StartEventContext context) override {
    // Extract the optional cf blob from the RPC params and pass it along with the
    // service channel to EventDispatcherImpl. The cf blob will be included in
    // SubrequestMetadata when creating the WorkerInterface for HTTP events.
    kj::Maybe<kj::String> cfBlobJson;
    auto params = context.getParams();
    if (params.hasCfBlobJson()) {
      cfBlobJson = kj::str(params.getCfBlobJson());
    }
    context.initResults(capnp::MessageSize{4, 1})
        .setDispatcher(kj::heap<EventDispatcherImpl>(
            httpOverCapnpFactory, kj::addRef(*service), kj::mv(cfBlobJson)));
    return kj::READY_NOW;
  }

 private:
  kj::Own<IoChannelFactory::SubrequestChannel> service;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;

  class EventDispatcherImpl final: public rpc::EventDispatcher::Server {
   public:
    EventDispatcherImpl(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
        kj::Own<IoChannelFactory::SubrequestChannel> service,
        kj::Maybe<kj::String> cfBlobJson)
        : httpOverCapnpFactory(httpOverCapnpFactory),
          service(kj::mv(service)),
          cfBlobJson(kj::mv(cfBlobJson)) {}

    kj::Promise<void> getHttpService(GetHttpServiceContext context) override {
      // Create WorkerInterface with cf blob metadata (if provided via startEvent).
      IoChannelFactory::SubrequestMetadata metadata;
      KJ_IF_SOME(cf, cfBlobJson) {
        metadata.cfBlobJson = kj::str(cf);
      }
      auto worker = getService()->startRequest(kj::mv(metadata));
      context.initResults(capnp::MessageSize{4, 1})
          .setHttp(httpOverCapnpFactory.kjToCapnp(kj::mv(worker)));
      return kj::READY_NOW;
    }

    kj::Promise<void> sendTraces(SendTracesContext context) override {
      auto traces =
          KJ_MAP(trace, context.getParams().getTraces()){ return kj::refcounted<Trace>(trace); };
      auto event = kj::heap<api::TraceCustomEvent>(api::TraceCustomEvent::TYPE, kj::mv(traces));
      auto worker = getWorker();
      auto result = co_await worker->customEvent(kj::mv(event));
      auto resp = context.getResults().getResult();
      resp.setOutcome(result.outcome);
    }

    kj::Promise<void> prewarm(PrewarmContext context) override {
      throwUnsupported();
    }

    kj::Promise<void> runScheduled(RunScheduledContext context) override {
      throwUnsupported();
    }

    kj::Promise<void> runAlarm(RunAlarmContext context) override {
      throwUnsupported();
    }

    kj::Promise<void> queue(QueueContext context) override {
      throwUnsupported();
    }

    kj::Promise<void> jsRpcSession(JsRpcSessionContext context) override {
      auto customEvent = kj::heap<api::JsRpcSessionCustomEvent>(
          api::JsRpcSessionCustomEvent::WORKER_RPC_EVENT_TYPE);

      auto cap = customEvent->getCap();
      capnp::PipelineBuilder<JsRpcSessionResults> pipelineBuilder;
      pipelineBuilder.setTopLevel(cap);
      context.setPipeline(pipelineBuilder.build());
      context.getResults().setTopLevel(kj::mv(cap));

      auto worker = getWorker();
      return worker->customEvent(kj::mv(customEvent)).ignoreResult().attach(kj::mv(worker));
    }

    kj::Promise<void> tailStreamSession(TailStreamSessionContext context) override {
      auto customEvent = kj::heap<tracing::TailStreamCustomEvent>();
      auto cap = customEvent->getCap();
      capnp::PipelineBuilder<TailStreamSessionResults> pipelineBuilder;
      pipelineBuilder.setTopLevel(cap);
      context.setPipeline(pipelineBuilder.build());
      context.getResults().setTopLevel(kj::mv(cap));

      auto worker = getWorker();
      auto result = co_await worker->customEvent(kj::mv(customEvent)).attach(kj::mv(worker));
      auto response = context.getResults();
      response.setResult(result.outcome);
    }

   private:
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
    kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> service;
    kj::Maybe<kj::String> cfBlobJson;

    kj::Own<IoChannelFactory::SubrequestChannel> getService() {
      auto result =
          kj::mv(KJ_ASSERT_NONNULL(service, "EventDispatcher can only be used for one request"));
      service = kj::none;
      return result;
    }

    kj::Own<WorkerInterface> getWorker() {
      // For non-HTTP events (RPC, traces, etc.), create WorkerInterface with
      // empty metadata since there's no HTTP request to extract cf from.
      return getService()->startRequest({});
    }

    [[noreturn]] void throwUnsupported() {
      JSG_FAIL_REQUIRE(Error, "RPC connections don't yet support this event type.");
    }
  };
};

class Server::HttpListener final: public kj::Refcounted {
 public:
  HttpListener(Server& owner,
      kj::Own<kj::ConnectionReceiver> listener,
      kj::Own<Service> service,
      kj::StringPtr physicalProtocol,
      kj::Own<HttpRewriter> rewriter,
      kj::HttpHeaderTable& headerTable,
      kj::Timer& timer,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
      : owner(owner),
        listener(kj::mv(listener)),
        service(kj::mv(service)),
        headerTable(headerTable),
        timer(timer),
        httpOverCapnpFactory(httpOverCapnpFactory),
        physicalProtocol(physicalProtocol),
        rewriter(kj::mv(rewriter)) {}

  kj::Promise<void> run() {
    TRACE_EVENT("workerd", "HttpListener::run");
    for (;;) {
      kj::AuthenticatedStream stream = co_await listener->acceptAuthenticated();
      TRACE_EVENT("workerd", "HTTPListener handle connection");

      kj::Maybe<kj::String> cfBlobJson;
      if (!rewriter->hasCfBlobHeader()) {
        // Construct a cf blob describing the client identity.

        kj::PeerIdentity* peerId;

        KJ_IF_SOME(tlsId,
            kj::dynamicDowncastIfAvailable<kj::TlsPeerIdentity>(*stream.peerIdentity)) {
          peerId = &tlsId.getNetworkIdentity();

          // TODO(someday): Add client certificate info to the cf blob? At present, KJ only
          //   supplies the common name, but that doesn't even seem to be one of the fields that
          //   Cloudflare-hosted Workers receive. We should probably try to match those.
        } else {
          peerId = stream.peerIdentity;
        }

        KJ_IF_SOME(remote, kj::dynamicDowncastIfAvailable<kj::NetworkPeerIdentity>(*peerId)) {
          cfBlobJson = kj::str("{\"clientIp\": ", escapeJsonString(remote.toString()), "}");
        } else KJ_IF_SOME(local, kj::dynamicDowncastIfAvailable<kj::LocalPeerIdentity>(*peerId)) {
          auto creds = local.getCredentials();

          kj::Vector<kj::String> parts;
          KJ_IF_SOME(p, creds.pid) {
            parts.add(kj::str("\"clientPid\":", p));
          }
          KJ_IF_SOME(u, creds.uid) {
            parts.add(kj::str("\"clientUid\":", u));
          }

          cfBlobJson = kj::str("{", kj::strArray(parts, ","), "}");
        }
      }

      auto conn = kj::heap<Connection>(*this, kj::mv(cfBlobJson));

      static auto constexpr listen = [](kj::Own<HttpListener> self, kj::Own<Connection> conn,
                                         kj::Own<kj::AsyncIoStream> stream) -> kj::Promise<void> {
        try {
          co_await conn->listedHttp.httpServer.listenHttp(kj::mv(stream));
        } catch (...) {
          KJ_LOG(ERROR, kj::getCaughtExceptionAsKj());
        }
      };

      // Run the connection handler loop in the global task set, so that run() waits for open
      // connections to finish before returning, even if the listener loop is canceled. However,
      // do not consider exceptions from a specific connection to be fatal.
      owner.tasks.add(listen(kj::addRef(*this), kj::mv(conn), kj::mv(stream.stream)));
    }
  }

 private:
  Server& owner;
  kj::Own<kj::ConnectionReceiver> listener;
  kj::Own<Service> service;
  kj::HttpHeaderTable& headerTable;
  kj::Timer& timer;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  kj::StringPtr physicalProtocol;
  kj::Own<HttpRewriter> rewriter;

  kj::Maybe<capnp::TwoPartyServer> capnpServer;

  kj::Promise<void> acceptCapnpConnection(kj::AsyncIoStream& conn) {
    KJ_IF_SOME(s, capnpServer) {
      return s.accept(conn);
    }

    // Capnp server not initialized. Create it now.
    auto& s = capnpServer.emplace(
        kj::heap<WorkerdBootstrapImpl>(kj::addRef(*service), httpOverCapnpFactory));
    return s.accept(conn);
  }

  struct Connection final: public kj::HttpService, public kj::HttpServerErrorHandler {
    Connection(HttpListener& parent, kj::Maybe<kj::String> cfBlobJson)
        : parent(parent),
          cfBlobJson(kj::mv(cfBlobJson)),
          webSocketErrorHandler(kj::heap<JsgifyWebSocketErrors>()),
          listedHttp(parent.owner,
              parent.timer,
              parent.headerTable,
              *this,
              kj::HttpServerSettings{.errorHandler = *this,
                .webSocketErrorHandler = *webSocketErrorHandler,
                .webSocketCompressionMode = kj::HttpServerSettings::MANUAL_COMPRESSION}) {}

    HttpListener& parent;
    kj::Maybe<kj::String> cfBlobJson;
    kj::Own<JsgifyWebSocketErrors> webSocketErrorHandler;
    ListedHttpServer listedHttp;

    class ResponseWrapper final: public kj::HttpService::Response {
     public:
      ResponseWrapper(kj::HttpService::Response& inner, HttpRewriter& rewriter)
          : inner(inner),
            rewriter(rewriter) {}

      kj::Own<kj::AsyncOutputStream> send(uint statusCode,
          kj::StringPtr statusText,
          const kj::HttpHeaders& headers,
          kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
        TRACE_EVENT("workerd", "ResponseWrapper::send()");
        auto rewrite = headers.cloneShallow();
        rewriter.rewriteResponse(rewrite);
        return inner.send(statusCode, statusText, rewrite, expectedBodySize);
      }

      kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
        TRACE_EVENT("workerd", "ResponseWrapper::acceptWebSocket()");
        auto rewrite = headers.cloneShallow();
        rewriter.rewriteResponse(rewrite);
        return inner.acceptWebSocket(rewrite);
      }

     private:
      kj::HttpService::Response& inner;
      HttpRewriter& rewriter;
    };

    // ---------------------------------------------------------------------------
    // implements kj::HttpService

    kj::Promise<void> request(kj::HttpMethod method,
        kj::StringPtr url,
        const kj::HttpHeaders& headers,
        kj::AsyncInputStream& requestBody,
        kj::HttpService::Response& response) override {
      TRACE_EVENT("workerd", "Connection:request()");
      IoChannelFactory::SubrequestMetadata metadata;
      metadata.cfBlobJson = mapCopyString(cfBlobJson);

      Response* wrappedResponse = &response;
      kj::Own<ResponseWrapper> ownResponse;
      if (parent.rewriter->needsRewriteResponse()) {
        wrappedResponse = ownResponse = kj::heap<ResponseWrapper>(response, *parent.rewriter);
      }

      if (parent.rewriter->needsRewriteRequest() || cfBlobJson != kj::none) {
        auto rewrite = KJ_UNWRAP_OR(parent.rewriter->rewriteIncomingRequest(
                                        url, parent.physicalProtocol, headers, metadata.cfBlobJson),
            { co_return co_await response.sendError(400, "Bad Request", parent.headerTable); });
        auto worker = parent.service->startRequest(kj::mv(metadata));
        co_return co_await worker->request(
            method, url, *rewrite.headers, requestBody, *wrappedResponse);
      } else {
        auto worker = parent.service->startRequest(kj::mv(metadata));
        co_return co_await worker->request(method, url, headers, requestBody, *wrappedResponse);
      }
    }

    kj::Promise<void> connect(kj::StringPtr host,
        const kj::HttpHeaders& headers,
        kj::AsyncIoStream& connection,
        ConnectResponse& response,
        kj::HttpConnectSettings settings) override {
      KJ_IF_SOME(h, parent.rewriter->getCapnpConnectHost()) {
        if (h == host) {
          // Client is requesting to open a capnp session!
          response.accept(200, "OK", kj::HttpHeaders(parent.headerTable));
          return parent.acceptCapnpConnection(connection);
        }
      }

      // TODO(someday): Deliver connect() event to to worker? For now we call the default
      //   implementation which throws an exception.
      return kj::HttpService::connect(host, headers, connection, response, kj::mv(settings));
    }

    // ---------------------------------------------------------------------------
    // implements kj::HttpServerErrorHandler

    kj::Promise<void> handleApplicationError(
        kj::Exception exception, kj::Maybe<kj::HttpService::Response&> response) override {
      if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
        // Don't send a response, just close connection.
        co_return;
      }
      KJ_LOG(ERROR, kj::str("Uncaught exception: ", exception));
      KJ_IF_SOME(r, response) {
        co_return co_await r.sendError(500, "Internal Server Error", parent.headerTable);
      }
    }
  };
};

kj::Promise<void> Server::listenHttp(kj::Own<kj::ConnectionReceiver> listener,
    kj::Own<Service> service,
    kj::StringPtr physicalProtocol,
    kj::Own<HttpRewriter> rewriter) {
  auto obj =
      kj::refcounted<HttpListener>(*this, kj::mv(listener), kj::mv(service), physicalProtocol,
          kj::mv(rewriter), globalContext->headerTable, timer, globalContext->httpOverCapnpFactory);
  co_return co_await obj->run();
}

// =======================================================================================
// Debug port for exposing all services via RPC

class Server::DebugPortListener {
 public:
  DebugPortListener(Server& owner,
      kj::Own<kj::ConnectionReceiver> listener,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
      : owner(owner),
        listener(kj::mv(listener)),
        httpOverCapnpFactory(httpOverCapnpFactory) {}

  kj::Promise<void> run() {
    capnp::TwoPartyServer server(kj::heap<WorkerdDebugPortImpl>(&owner, httpOverCapnpFactory));
    co_return co_await server.listen(*listener);
  }

 private:
  Server& owner;
  kj::Own<kj::ConnectionReceiver> listener;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;

  class WorkerdDebugPortImpl final: public rpc::WorkerdDebugPort::Server {
   public:
    WorkerdDebugPortImpl(
        workerd::server::Server* srvPtr, capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
        : srv(*srvPtr),
          httpOverCapnpFactory(httpOverCapnpFactory) {}

    kj::Promise<void> getEntrypoint(GetEntrypointContext context) override {
      auto params = context.getParams();
      auto serviceName = params.getService();
      auto propsReader = params.getProps();

      // Look up the service
      auto& serviceEntry =
          KJ_ASSERT_NONNULL(srv.services.find(serviceName), "Service not found", serviceName);
      auto service = serviceEntry->service();

      // Convert props from Frankenvalue if provided
      Frankenvalue props;
      if (params.hasProps()) {
        props = Frankenvalue::fromCapnp(propsReader);
      }

      kj::Own<Service> targetService;

      // Try to cast to WorkerService to support entrypoints and props
      auto* workerService = dynamic_cast<WorkerService*>(service);
      if (workerService != nullptr) {
        // This is a WorkerService, use getEntrypoint which supports both entrypoints and props
        kj::Maybe<kj::StringPtr> maybeEntrypoint;
        if (params.hasEntrypoint()) {
          maybeEntrypoint = params.getEntrypoint();
        }

        targetService =
            KJ_ASSERT_NONNULL(workerService->getEntrypoint(maybeEntrypoint, kj::mv(props)),
                "Entrypoint not found", maybeEntrypoint.orDefault("(default)"));
      } else {
        // Not a WorkerService
        KJ_ASSERT(
            !params.hasEntrypoint(), "Service does not support named entrypoints", serviceName);

        // Try to apply props if the service supports it
        if (params.hasProps()) {
          targetService = service->forProps(kj::mv(props));
        } else {
          // No props, just use the service as-is
          targetService = kj::addRef(*service);
        }
      }

      // Return a WorkerdBootstrap that wraps this service using the generic implementation.
      context.initResults(capnp::MessageSize{4, 1})
          .setEntrypoint(
              kj::heap<WorkerdBootstrapImpl>(kj::mv(targetService), httpOverCapnpFactory));
      return kj::READY_NOW;
    }

    kj::Promise<void> getActor(GetActorContext context) override {
      auto params = context.getParams();
      auto serviceName = params.getService();
      auto entrypointName = params.getEntrypoint();
      auto actorIdStr = params.getActorId();

      // Look up the service
      auto& serviceEntry =
          KJ_ASSERT_NONNULL(srv.services.find(serviceName), "Service not found", serviceName);
      auto service = serviceEntry->service();

      // Try to cast to WorkerService
      auto* workerService = dynamic_cast<WorkerService*>(service);
      KJ_REQUIRE(workerService != nullptr, "Service does not support actors", serviceName);

      // Look up the actor namespace
      auto& actorNamespace = KJ_ASSERT_NONNULL(workerService->getActorNamespace(entrypointName),
          "Actor namespace not found", entrypointName);

      // Create an actor ID - use the namespace config to determine if it's durable or ephemeral
      Worker::Actor::Id actorId;
      KJ_SWITCH_ONEOF(actorNamespace.getConfig()) {
        KJ_CASE_ONEOF(c, Durable) {
          // Durable Object ID (hex-encoded SHA256 hash)
          auto decoded = kj::decodeHex(actorIdStr);
          KJ_REQUIRE(decoded.size() == SHA256_DIGEST_LENGTH,
              "Invalid Durable Object ID: expected 64 hex characters (32 bytes)", decoded.size());
          kj::Own<ActorIdFactory::ActorId> id =
              kj::heap<ActorIdFactoryImpl::ActorIdImpl>(decoded.begin(), kj::none);
          actorId = kj::mv(id);
        }
        KJ_CASE_ONEOF(c, Ephemeral) {
          // Ephemeral actor ID (plain string)
          actorId = kj::str(actorIdStr);
        }
      }

      // Wrap the actor channel using the generic WorkerdBootstrap implementation.
      context.initResults(capnp::MessageSize{4, 1})
          .setActor(kj::heap<WorkerdBootstrapImpl>(
              actorNamespace.getActorChannel(kj::mv(actorId)), httpOverCapnpFactory));
      return kj::READY_NOW;
    }

   private:
    workerd::server::Server& srv;
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  };
};

kj::Promise<void> Server::listenDebugPort(kj::Own<kj::ConnectionReceiver> listener) {
  DebugPortListener obj(*this, kj::mv(listener), globalContext->httpOverCapnpFactory);
  co_return co_await obj.run();
}

// =======================================================================================
// Server::run()

kj::Promise<void> Server::handleDrain(kj::Promise<void> drainWhen) {
  co_await drainWhen;
  TRACE_EVENT("workerd", "Server::handleDrain()");
  // Tell all HttpServers to drain. This causes them to disconnect any connections that don't
  // have a request in-flight.
  for (auto& httpServer: httpServers) {
    // The promise returned by `drain()` resolves when all connections have ended. But, we need
    // the promise returned by handleDrain() to resolve immediately when draining has started,
    // since that's what signals us to stop accepting incoming connections. So, we should not
    // co_await the promise returned by `drain()`. Technically, we don't actually have to wait
    // on it at all -- `drain()` returns the promise end of a promise-and-fulfiller, so simply
    // dropping it won't actually cancel anything. But since that's not documented in drain()'s
    // doc comment, we instead add the promise to `tasks` to be safe.
    tasks.add(httpServer.httpServer.drain());
  }
}

kj::Promise<void> Server::run(
    jsg::V8System& v8System, config::Config::Reader config, kj::Promise<void> drainWhen) {
  TRACE_EVENT("workerd", "Server.run");

  // Update logging settings from config (overridding structuredLogging when so)
  if (config.hasLogging()) {
    auto logging = config.getLogging();
    loggingOptions.structuredLogging = StructuredLogging(logging.getStructuredLogging());
    if (logging.hasStdoutPrefix()) {
      loggingOptions.stdoutPrefix = kj::ConstString(kj::str(logging.getStdoutPrefix()));
    }
    if (logging.hasStderrPrefix()) {
      loggingOptions.stderrPrefix = kj::ConstString(kj::str(logging.getStderrPrefix()));
    }
  } else {
    loggingOptions.structuredLogging = StructuredLogging(config.getStructuredLogging());
  }

  kj::HttpHeaderTable::Builder headerTableBuilder;
  globalContext = kj::heap<GlobalContext>(*this, v8System, headerTableBuilder);
  invalidConfigServiceSingleton = kj::refcounted<InvalidConfigService>();
  invalidConfigActorClassSingleton = kj::refcounted<InvalidConfigActorClass>();

  auto [fatalPromise, fatalFulfiller] = kj::newPromiseAndFulfiller<void>();
  this->fatalFulfiller = kj::mv(fatalFulfiller);

  auto forkedDrainWhen = handleDrain(kj::mv(drainWhen)).fork();

  co_await startServices(v8System, config, headerTableBuilder, forkedDrainWhen);

  auto listenPromise = listenOnSockets(config, headerTableBuilder, forkedDrainWhen);

  // We should have registered all headers synchronously. This is important because we want to
  // be able to start handling requests as soon as the services are available, even if some other
  // services take longer to get ready.
  auto ownHeaderTable = headerTableBuilder.build();

  co_return co_await listenPromise.exclusiveJoin(kj::mv(fatalPromise));
}

void Server::startAlarmScheduler(config::Config::Reader config) {
  auto& clock = kj::systemPreciseCalendarClock();
  auto dir = kj::newInMemoryDirectory(clock);
  auto vfs = kj::heap<SqliteDatabase::Vfs>(*dir).attach(kj::mv(dir));

  // TODO(someday): support persistent storage for alarms

  alarmScheduler =
      kj::heap<AlarmScheduler>(clock, timer, *vfs, kj::Path({"alarms.sqlite"})).attach(kj::mv(vfs));
}

// Configure and start the inspector socket, returning the port the socket started on.
uint startInspector(
    kj::StringPtr inspectorAddress, Server::InspectorServiceIsolateRegistrar& registrar) {
  static constexpr uint UNASSIGNED_PORT = 0;
  static constexpr uint DEFAULT_PORT = 9229;
  kj::MutexGuarded<uint> inspectorPort(UNASSIGNED_PORT);

  // `startInspector()` is called on the Isolate thread. V8 requires CPU profiling to be started and
  // stopped on the same thread which executes JavaScript -- that is, the Isolate thread -- which
  // means we need to dispatch inspector messages on this thread. To help make that happen, we
  // capture this thread's kj::Executor here, and pass it into the InspectorService below. Later,
  // when the InspectorService receives a WebSocket connection, it calls
  // `Isolate::attachInspector()`, which uses the kj::Executor we create here to create a
  // XThreadNotifier and start a dispatch loop. The InspectorService reads subsequent WebSocket
  // inspector messages and feeds them to that dispatch loop via the XThreadNotifier.
  auto isolateThreadExecutor = kj::getCurrentThreadExecutor().addRef();

  // Start the InspectorService thread.
  kj::Thread thread([inspectorAddress, &inspectorPort, &registrar,
                        isolateThreadExecutor = kj::mv(isolateThreadExecutor)]() mutable {
    kj::AsyncIoContext io = kj::setupAsyncIo();

    kj::HttpHeaderTable::Builder headerTableBuilder;

    // Create the special inspector service.
    auto inspectorService(kj::heap<Server::InspectorService>(
        kj::mv(isolateThreadExecutor), io.provider->getTimer(), headerTableBuilder, registrar));
    auto ownHeaderTable = headerTableBuilder.build();

    // Configure and start the inspector socket.

    auto& network = io.provider->getNetwork();

    // TODO(cleanup): There's an issue here that if listen fails, nothing notices. The
    // server will continue running but will no longer accept inspector connections.
    // This should be fixed by:
    // 1. Replacing the kj::NEVER_DONE with listen
    // 2. Making the thread's lambda `noexcept` so that if it throws the process crashes
    // 3. Probably also throw if listen completes without an exception (even if unlikely to
    //    happen)
    auto listen = (kj::coCapture(
        [&network, &inspectorAddress, &inspectorPort, &inspectorService]() -> kj::Promise<void> {
      auto parsed = co_await network.parseAddress(inspectorAddress, DEFAULT_PORT);
      auto listener = parsed->listen();
      // EW-7716: Signal to thread that started the inspector service that the inspector is ready.
      *inspectorPort.lockExclusive() = listener->getPort();
      KJ_LOG(INFO, "Inspector is listening");
      co_await inspectorService->listen(kj::mv(listener));
    }))();

    kj::NEVER_DONE.wait(io.waitScope);
  });
  thread.detach();

  // EW-7716: Wait for the InspectorService instance to be initialized before proceeding.
  return inspectorPort.when([](const uint& port) { return port != UNASSIGNED_PORT; },
      [](const uint& port) { return port; });
}

kj::Promise<void> Server::preloadPython(
    kj::StringPtr workerName, const WorkerDef& workerDef, ErrorReporter& errorReporter) {
  if (workerDef.featureFlags.getPythonWorkers()) {
    auto pythonRelease = getPythonSnapshotRelease(workerDef.featureFlags);
    KJ_IF_SOME(release, pythonRelease) {
      auto version = getPythonBundleName(release);

      // Fetch the Pyodide bundle.
      co_await server::fetchPyodideBundle(pythonConfig, kj::mv(version), network, timer);

      // Preload Python packages.
      KJ_IF_SOME(modulesSource, workerDef.source.variant.tryGet<Worker::Script::ModulesSource>()) {
        if (modulesSource.isPython) {
          auto pythonRequirements = getPythonRequirements(modulesSource);

          // Store the packages in the package manager that is stored in the pythonConfig
          co_await server::fetchPyodidePackages(pythonConfig, pythonConfig.pyodidePackageManager,
              pythonRequirements, release, network, timer);
        }
      }
    }
  }
}

kj::Promise<void> Server::startServices(jsg::V8System& v8System,
    config::Config::Reader config,
    kj::HttpHeaderTable::Builder& headerTableBuilder,
    kj::ForkedPromise<void>& forkedDrainWhen) {
  // ---------------------------------------------------------------------------
  // Configure services
  TRACE_EVENT("workerd", "startServices");

  // First pass: Extract actor namespace configs.
  for (auto serviceConf: config.getServices()) {
    kj::StringPtr name = serviceConf.getName();
    kj::HashMap<kj::String, ActorConfig> serviceActorConfigs;

    if (serviceConf.isWorker()) {
      auto workerConf = serviceConf.getWorker();
      bool hadDurable = false;
      for (auto ns: workerConf.getDurableObjectNamespaces()) {
        switch (ns.which()) {
          case config::Worker::DurableObjectNamespace::UNIQUE_KEY:
            hadDurable = true;
            serviceActorConfigs.insert(kj::str(ns.getClassName()),
                Durable{.uniqueKey = kj::str(ns.getUniqueKey()),
                  .isEvictable = !ns.getPreventEviction(),
                  .enableSql = ns.getEnableSql(),
                  .containerOptions = ns.hasContainer() ? kj::Maybe(ns.getContainer()) : kj::none});
            continue;
          case config::Worker::DurableObjectNamespace::EPHEMERAL_LOCAL:
            if (!experimental) {
              reportConfigError(kj::str(
                  "Ephemeral objects (Durable Object namespaces with type 'ephemeralLocal') are an "
                  "experimental feature which may change or go away in the future. You must run "
                  "workerd with `--experimental` to use this feature."));
            }
            serviceActorConfigs.insert(kj::str(ns.getClassName()),
                Ephemeral{.isEvictable = !ns.getPreventEviction(), .enableSql = ns.getEnableSql()});
            continue;
        }
        reportConfigError(kj::str("Encountered unknown DurableObjectNamespace type in service \"",
            name, "\", class \"", ns.getClassName(),
            "\". Was the config compiled with a newer version "
            "of the schema?"));
      }

      switch (workerConf.getDurableObjectStorage().which()) {
        case config::Worker::DurableObjectStorage::NONE:
          if (hadDurable) {
            reportConfigError(kj::str("Worker service \"", name,
                "\" implements durable object classes but has "
                "`durableObjectStorage` set to `none`."));
          }
          goto validDurableObjectStorage;
        case config::Worker::DurableObjectStorage::IN_MEMORY:
        case config::Worker::DurableObjectStorage::LOCAL_DISK:
          goto validDurableObjectStorage;
      }
      reportConfigError(kj::str("Encountered unknown durableObjectStorage type in service \"", name,
          "\". Was the config compiled with a newer version of the schema?"));

    validDurableObjectStorage:
      if (workerConf.hasDurableObjectUniqueKeyModifier()) {
        // This should be implemented along with parameterized workers. It's not relevant
        // otherwise, but let's make sure no one sets it accidentally.
        KJ_UNIMPLEMENTED("durableObjectUniqueKeyModifier is not implemented yet");
      }
    }

    actorConfigs.upsert(kj::str(name), kj::mv(serviceActorConfigs), [&](auto&&...) {
      reportConfigError(kj::str("Config defines multiple services named \"", name, "\"."));
    });
  }

  // If we are using the inspector, we need to register the Worker::Isolate
  // with the inspector service.
  KJ_IF_SOME(inspectorAddress, inspectorOverride) {
    auto registrar = kj::heap<InspectorServiceIsolateRegistrar>();
    auto port = startInspector(inspectorAddress, *registrar);
    KJ_IF_SOME(stream, controlOverride) {
      auto message = kj::str("{\"event\":\"listen-inspector\",\"port\":", port, "}\n");
      try {
        stream->write(message.asBytes());
      } catch (kj::Exception& e) {
        KJ_LOG(ERROR, e);
      }
    }
    inspectorIsolateRegistrar = kj::mv(registrar);
  }

  // Second pass: Build services.
  for (auto serviceConf: config.getServices()) {
    kj::StringPtr name = serviceConf.getName();
    auto service = co_await makeService(serviceConf, headerTableBuilder, config.getExtensions());

    services.upsert(kj::str(name), kj::mv(service), [&](auto&&...) {
      reportConfigError(kj::str("Config defines multiple services named \"", name, "\"."));
    });
  }

  // Make the default "internet" service if it's not there already.
  services.findOrCreate("internet"_kj, [&]() {
    auto publicNetwork = network.restrictPeers({"public"_kj});

    kj::TlsContext::Options options;
    options.useSystemTrustStore = true;

    kj::Own<kj::TlsContext> tls = kj::heap<kj::TlsContext>(kj::mv(options));
    auto tlsNetwork = tls->wrapNetwork(*publicNetwork);

    // Attaching to refcounted NetworkService is safe since services map is long-lived
    auto service = kj::refcounted<NetworkService>(globalContext->headerTable, timer, entropySource,
        kj::mv(publicNetwork), kj::mv(tlsNetwork), *tls)
                       .attachToThisReference(kj::mv(tls));

    return decltype(services)::Entry{kj::str("internet"_kj), kj::mv(service)};
  });

  // Start the alarm scheduler before linking services
  startAlarmScheduler(config);

  // Third pass: Cross-link services.
  for (auto& service: services) {
    ConfigErrorReporter errorReporter(*this, service.key);
    service.value->link(errorReporter);
  }
}

kj::Promise<void> Server::listenOnSockets(config::Config::Reader config,
    kj::HttpHeaderTable::Builder& headerTableBuilder,
    kj::ForkedPromise<void>& forkedDrainWhen,
    bool forTest) {
  // ---------------------------------------------------------------------------
  // Start sockets
  TRACE_EVENT("workerd", "listenOnSockets");
  for (auto sock: config.getSockets()) {
    kj::StringPtr name = sock.getName();
    kj::StringPtr addrStr = nullptr;
    kj::String ownAddrStr;
    kj::Maybe<kj::Own<kj::ConnectionReceiver>> listenerOverride;

    kj::Own<Service> service = lookupService(sock.getService(), kj::str("Socket \"", name, "\""));

    KJ_IF_SOME(override, socketOverrides.findEntry(name)) {
      KJ_SWITCH_ONEOF(override.value) {
        KJ_CASE_ONEOF(str, kj::String) {
          addrStr = ownAddrStr = kj::mv(str);
          break;
        }
        KJ_CASE_ONEOF(l, kj::Own<kj::ConnectionReceiver>) {
          listenerOverride = kj::mv(l);
          break;
        }
      }
      socketOverrides.erase(override);
    } else if (sock.hasAddress()) {
      addrStr = sock.getAddress();
    } else {
      reportConfigError(kj::str("Socket \"", name,
          "\" has no address in the config, so must be specified on the "
          "command line with `--socket-addr`."));
      continue;
    }

    uint defaultPort = 0;
    config::HttpOptions::Reader httpOptions;
    kj::Maybe<kj::Own<kj::TlsContext>> tls;
    kj::StringPtr physicalProtocol;
    switch (sock.which()) {
      case config::Socket::HTTP:
        defaultPort = 80;
        httpOptions = sock.getHttp();
        physicalProtocol = "http";
        goto validSocket;
      case config::Socket::HTTPS: {
        auto https = sock.getHttps();
        defaultPort = 443;
        httpOptions = https.getOptions();
        tls = makeTlsContext(https.getTlsOptions());
        physicalProtocol = "https";
        goto validSocket;
      }
    }
    reportConfigError(kj::str("Encountered unknown socket type in \"", name,
        "\". Was the config compiled with a "
        "newer version of the schema?"));
    continue;

  validSocket:
    using PromisedReceived = kj::Promise<kj::Own<kj::ConnectionReceiver>>;
    PromisedReceived listener = nullptr;
    KJ_IF_SOME(l, listenerOverride) {
      listener = kj::mv(l);
    } else {
      listener = ([](kj::Promise<kj::Own<kj::NetworkAddress>> promise) -> PromisedReceived {
        auto parsed = co_await promise;
        co_return parsed->listen();
      })(network.parseAddress(addrStr, defaultPort));
    }

    KJ_IF_SOME(t, tls) {
      listener = ([](kj::Promise<kj::Own<kj::ConnectionReceiver>> promise,
                      kj::Own<kj::TlsContext> tls) -> PromisedReceived {
        auto port = co_await promise;
        co_return tls->wrapPort(kj::mv(port)).attach(kj::mv(tls));
      })(kj::mv(listener), kj::mv(t));
    }

    // Need to create rewriter before waiting on anything since `headerTableBuilder` will no longer
    // be available later.
    auto rewriter = kj::heap<HttpRewriter>(httpOptions, headerTableBuilder);

    auto handle = kj::coCapture(
        [this, service = kj::mv(service), rewriter = kj::mv(rewriter), physicalProtocol, name](
            kj::Promise<kj::Own<kj::ConnectionReceiver>> promise) mutable -> kj::Promise<void> {
      TRACE_EVENT("workerd", "setup listenHttp");
      auto listener = co_await promise;
      KJ_IF_SOME(stream, controlOverride) {
        auto message = kj::str("{\"event\":\"listen\",\"socket\":\"", name,
            "\",\"port\":", listener->getPort(), "}\n");
        try {
          stream->write(message.asBytes());
        } catch (kj::Exception& e) {
          KJ_LOG(ERROR, e);
        }
      }
      co_await listenHttp(kj::mv(listener), kj::mv(service), physicalProtocol, kj::mv(rewriter));
    });
    tasks.add(handle(kj::mv(listener)).exclusiveJoin(forkedDrainWhen.addBranch()));
  }

  // Start debug port if configured
  KJ_IF_SOME(addr, debugPortOverride) {
    auto handle = kj::coCapture(
        [this, addr = kj::str(addr)](kj::ForkedPromise<void>& drain) mutable -> kj::Promise<void> {
      auto parsed = co_await network.parseAddress(addr, 0);
      auto listener = parsed->listen();

      KJ_IF_SOME(stream, controlOverride) {
        auto message = kj::str("{\"event\":\"listen\",\"socket\":\"debug-port"
                               "\",\"port\":",
            listener->getPort(), "}\n");
        try {
          stream->write(message.asBytes());
        } catch (kj::Exception& e) {
          KJ_LOG(ERROR, e);
        }
      }

      co_await listenDebugPort(kj::mv(listener));
    });
    tasks.add(handle(forkedDrainWhen).exclusiveJoin(forkedDrainWhen.addBranch()));
  }

  for (auto& unmatched: socketOverrides) {
    reportConfigError(kj::str("Config did not define any socket named \"", unmatched.key,
        "\" to match the override "
        "provided on the command line."));
  }

  for (auto& unmatched: externalOverrides) {
    reportConfigError(kj::str("Config did not define any external service named \"", unmatched.key,
        "\" to match the "
        "override provided on the command line."));
  }

  for (auto& unmatched: directoryOverrides) {
    if (forTest && unmatched.key == "TEST_TMPDIR") {
      // Due to a historical bug, `workerd test` didn't check for the existence of unmatched
      // overrides, and our own tests became dependent on the ability to override TEST_TMPDIR
      // even if it was not used in the config. For now, we ignore this problem.
      //
      // TODO(cleanup): Figure out the right solution here.
      continue;
    }

    reportConfigError(kj::str("Config did not define any disk service named \"", unmatched.key,
        "\" to match the "
        "override provided on the command line."));
  }

  co_await tasks.onEmpty();

  // Give a chance for any errors to bubble up before we return success. In particular
  // Server::taskFailed() fulfills `fatalFulfiller`, which causes the server to exit with an error.
  // But the `TaskSet` may have become empty at the same time. We want the error to win the race
  // against the success.
  //
  // TODO(cleanup): A better solution would be for `TaskSet` to have a new variant of the
  //   `onEmpty()` method like `onEmptyOrException()`, which propagates any exception thrown by
  //   any task.
  co_await kj::yieldUntilQueueEmpty();
}

// =======================================================================================
// Server::test()

kj::Promise<bool> Server::test(jsg::V8System& v8System,
    config::Config::Reader config,
    kj::StringPtr servicePattern,
    kj::StringPtr entrypointPattern) {

  if (config.hasLogging()) {
    auto logging = config.getLogging();
    loggingOptions.structuredLogging = StructuredLogging(logging.getStructuredLogging());
    if (logging.hasStdoutPrefix()) {
      loggingOptions.stdoutPrefix = kj::ConstString(kj::str(logging.getStdoutPrefix()));
    }
    if (logging.hasStderrPrefix()) {
      loggingOptions.stderrPrefix = kj::ConstString(kj::str(logging.getStderrPrefix()));
    }
  } else {
    loggingOptions.structuredLogging = StructuredLogging(config.getStructuredLogging());
  }

  kj::HttpHeaderTable::Builder headerTableBuilder;
  globalContext = kj::heap<GlobalContext>(*this, v8System, headerTableBuilder);
  invalidConfigServiceSingleton = kj::refcounted<InvalidConfigService>();

  auto [fatalPromise, fatalFulfiller] = kj::newPromiseAndFulfiller<void>();
  this->fatalFulfiller = kj::mv(fatalFulfiller);

  auto forkedDrainWhen = kj::Promise<void>(kj::NEVER_DONE).fork();

  co_await startServices(v8System, config, headerTableBuilder, forkedDrainWhen);

  // Tests usually do not configure sockets, but they can, especially loopback sockets. Arrange
  // to wait on them. Crash if listening fails.
  auto listenPromise =
      listenOnSockets(config, headerTableBuilder, forkedDrainWhen,
          /* forTest = */ true)
          .eagerlyEvaluate([](kj::Exception&& e) noexcept { kj::throwFatalException(kj::mv(e)); });

  auto ownHeaderTable = headerTableBuilder.build();

  // TODO(someday): If the inspector is enabled, pause and wait for an inspector connection before
  //   proceeding?

  kj::GlobFilter serviceGlob(servicePattern);
  kj::GlobFilter entrypointGlob(entrypointPattern);

  uint passCount = 0, failCount = 0;

  auto doTest = [&](Service& service, kj::StringPtr name) -> kj::Promise<void> {
    // TODO(soon): Better way of reporting test results, KJ_LOG is ugly. We should probably have
    //   some sort of callback interface. It would be nice to report the exceptions thrown through
    //   that interface too... can we? Use a tracer maybe?
    // HACK: We use DBG log level because INFO logging is optional, and warning/error would confuse
    //   people. Note that server-test.c++ actually tests for this logging, so simply writing to
    //   stderr wouldn't work.
    KJ_LOG(DBG, kj::str("[ TEST ] "_kj, name));
    auto req = service.startRequest({});
    auto start = monotonicClock.now();

    bool result = co_await req->test();
    if (result) {
      ++passCount;
    } else {
      ++failCount;
    }

    auto end = monotonicClock.now();
    auto duration = end - start;

    KJ_LOG(DBG, kj::str(result ? "[ PASS ] "_kj : "[ FAIL ] "_kj, name, " (", duration, ")"));
  };

  for (auto& service: services) {
    if (serviceGlob.matches(service.key)) {
      if (service.value->hasHandler("test"_kj) && entrypointGlob.matches("default"_kj)) {
        co_await doTest(*service.value, service.key);
      }

      if (WorkerService* worker = dynamic_cast<WorkerService*>(service.value.get())) {
        for (auto& name: worker->getEntrypointNames()) {
          if (entrypointGlob.matches(name)) {
            kj::Own<Service> ep = KJ_ASSERT_NONNULL(worker->getEntrypoint(name, /*props=*/{}));
            if (ep->hasHandler("test"_kj)) {
              co_await doTest(*ep, kj::str(service.key, ':', name));
            }
          }
        }
      }
    }
  }

  if (passCount + failCount == 0) {
    KJ_LOG(ERROR, "No tests found!");
  }

  co_return passCount > 0 && failCount == 0;
}

}  // namespace workerd::server
