// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "server.h"
#include <kj/debug.h>
#include <kj/glob-filter.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/compat/url.h>
#include <kj/encoding.h>
#include <kj/map.h>
#include <capnp/message.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/compat/json.h>
#include <workerd/api/analytics-engine.capnp.h>
#include <workerd/io/actor-id.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker-entrypoint.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <time.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <workerd/io/actor-cache.h>
#include <workerd/io/actor-sqlite.h>
#include <workerd/io/request-tracker.h>
#include <workerd/util/http-util.h>
#include <workerd/api/actor-state.h>
#include <workerd/util/mimetype.h>
#include <workerd/util/uuid.h>
#include <workerd/util/use-perfetto-categories.h>
#include <workerd/api/worker-rpc.h>
#include "workerd-api.h"
#include "workerd/io/hibernation-manager.h"
#include <stdlib.h>

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
    void disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                     size_t capacity, void (*destroyElement)(void*)) const override {
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

  return PemData { kj::String(kj::mv(nameArr)), kj::mv(data) };
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
  char buf[256];
  size_t n = strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  KJ_ASSERT(n > 0);
  return kj::heapString(buf, n);
}

static kj::Vector<char> escapeJsonString(kj::StringPtr text) {
  static const char HEXDIGITS[] = "0123456789abcdef";
  kj::Vector<char> escaped(text.size() + 1);

  for (char c: text) {
    switch (c) {
      case '\"': escaped.addAll(kj::StringPtr("\\\"")); break;
      case '\\': escaped.addAll(kj::StringPtr("\\\\")); break;
      case '\b': escaped.addAll(kj::StringPtr("\\b")); break;
      case '\f': escaped.addAll(kj::StringPtr("\\f")); break;
      case '\n': escaped.addAll(kj::StringPtr("\\n")); break;
      case '\r': escaped.addAll(kj::StringPtr("\\r")); break;
      case '\t': escaped.addAll(kj::StringPtr("\\t")); break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          escaped.addAll(kj::StringPtr("\\u00"));
          uint8_t c2 = c;
          escaped.add(HEXDIGITS[c2 / 16]);
          escaped.add(HEXDIGITS[c2 % 16]);
        } else {
          escaped.add(c);
        }
        break;
    }
  }

  return escaped;
}

}  // namespace

// =======================================================================================

Server::Server(kj::Filesystem& fs, kj::Timer& timer, kj::Network& network,
               kj::EntropySource& entropySource, Worker::ConsoleMode consoleMode,
               kj::Function<void(kj::String)> reportConfigError)
    : fs(fs), timer(timer), network(network), entropySource(entropySource),
      reportConfigError(kj::mv(reportConfigError)), consoleMode(consoleMode),
      memoryCacheProvider(kj::heap<api::MemoryCacheProvider>()), tasks(*this) {}

Server::~Server() noexcept(false) {}

struct Server::GlobalContext {
  jsg::V8System& v8System;
  capnp::ByteStreamFactory byteStreamFactory;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;
  kj::HttpHeaderTable& headerTable;

  GlobalContext(Server& server, jsg::V8System& v8System,
                kj::HttpHeaderTable::Builder& headerTableBuilder)
      : v8System(v8System),
        httpOverCapnpFactory(byteStreamFactory, headerTableBuilder,
                             capnp::HttpOverCapnpFactory::LEVEL_2),
        threadContext(server.timer, server.entropySource,
            headerTableBuilder, httpOverCapnpFactory,
            byteStreamFactory,
            false /* isFiddle -- TODO(beta): support */),
        headerTable(headerTableBuilder.getFutureTable()) {}
};

class Server::Service {
public:
  // Cross-links this service with other services. Must be called once before `startRequest()`.
  virtual void link() {}

  // Begin an incoming request. Returns a `WorkerInterface` object that will be used for one
  // request then discarded.
  virtual kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata) = 0;

  // Returns true if the service exports the given handler, e.g. `fetch`, `scheduled`, etc.
  virtual bool hasHandler(kj::StringPtr handlerName) = 0;
};

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
    options.defaultKeypair = attachments->keypair.emplace(kj::TlsKeypair {
      .privateKey = kj::TlsPrivateKey(pairConf.getPrivateKey()),
      .certificate = kj::TlsCertificate(pairConf.getCertificateChain())
    });
  }

  options.verifyClients = conf.getRequireClientCerts();
  options.useSystemTrustStore = conf.getTrustBrowserCas();

  auto trustList = conf.getTrustedCertificates();
  if (trustList.size() > 0) {
    attachments->trustedCerts = KJ_MAP(cert, trustList) {
      return kj::TlsCertificate(cert);
    };
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
    config::TlsOptions::Reader conf, kj::StringPtr addrStr,
    kj::Maybe<kj::StringPtr> certificateHost, uint defaultPort) {
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
  HttpRewriter(config::HttpOptions::Reader httpOptions,
               kj::HttpHeaderTable::Builder& headerTableBuilder)
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
    return style == config::HttpOptions::Style::HOST
        || hasCfBlobHeader()
        || !requestInjector.empty();
  }

  // Attach this to the promise returned by request().
  struct Rewritten {
    kj::Own<kj::HttpHeaders> headers;
    kj::String ownUrl;
  };

  Rewritten rewriteOutgoingRequest(kj::StringPtr& url, const kj::HttpHeaders& headers,
                                   kj::Maybe<kj::StringPtr> cfBlobJson) {
    Rewritten result { kj::heap(headers.cloneShallow()), nullptr };

    if (style == config::HttpOptions::Style::HOST) {
      auto parsed = kj::Url::parse(url, kj::Url::HTTP_PROXY_REQUEST,
          kj::Url::Options {.percentDecode = false, .allowEmpty = true});
      result.headers->set(kj::HttpHeaderId::HOST, kj::mv(parsed.host));
      KJ_IF_SOME(h, forwardedProtoHeader) {
        result.headers->set(h, kj::mv(parsed.scheme));
      }
      url = result.ownUrl = parsed.toString(kj::Url::HTTP_REQUEST);
    }

    KJ_IF_SOME(h, cfBlobHeader) {
      KJ_IF_SOME(b, cfBlobJson) {
        result.headers->set(h, b);
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
    Rewritten result { kj::heap(headers.cloneShallow()), nullptr };

    if (style == config::HttpOptions::Style::HOST) {
      auto parsed = kj::Url::parse(url, kj::Url::HTTP_REQUEST,
          kj::Url::Options {.percentDecode = false, .allowEmpty = true});
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

    bool empty() { return injectedHeaders.size() == 0; }

    void apply(kj::HttpHeaders& headers) {
      for (auto& header: injectedHeaders) {
        KJ_IF_SOME(v, header.value) {
          headers.set(header.id, v);
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

// Return a fake Own pointing to the singleton.
kj::Own<Server::Service> Server::makeInvalidConfigService() {
  return { invalidConfigServiceSingleton.get(), kj::NullDisposer::instance };
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
      : promise(promise.then([this](kj::Own<kj::NetworkAddress> result) {
          addr = kj::mv(result);
        }).fork()) {}

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
  ExternalTcpService(kj::Own<kj::NetworkAddress> addrParam)
    : addr(kj::mv(addrParam)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return { this, kj::NullDisposer::instance };
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    return handlerName == "fetch"_kj || handlerName == "connect"_kj;
  }

private:
  kj::Own<kj::NetworkAddress> addr;

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override {
    throwUnsupported();
  }

  kj::Promise<void> connect(
      kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
      ConnectResponse& tunnel, kj::HttpConnectSettings settings) override {
      TRACE_EVENT("workerd", "ExternalTcpService::connect()", "host", host.cStr());
      auto io_stream = co_await addr->connect();

      auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);

      promises.add(connection.pumpTo(*io_stream).then([&io_stream=*io_stream](uint64_t size) {
        io_stream.shutdownWrite();
      }));

      promises.add(io_stream->pumpTo(connection).then([&connection](uint64_t size) {
        connection.shutdownWrite();
      }));

      tunnel.accept(200, "OK", kj::HttpHeaders(kj::HttpHeaderTable{}));

      co_await kj::joinPromisesFailFast(promises.finish()).attach(kj::mv(io_stream));
  }

  void prewarm(kj::StringPtr url) override {}
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    throwUnsupported();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    throwUnsupported();
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    throwUnsupported();
  }

  [[noreturn]] void throwUnsupported() {
    JSG_FAIL_REQUIRE(Error, "External TCP servers don't support this event type.");
  }

};

// Service used when the service is configured as external HTTP service.
class Server::ExternalHttpService final: public Service, private kj::TaskSet::ErrorHandler {
public:
  ExternalHttpService(kj::Own<kj::NetworkAddress> addrParam,
                      kj::Own<HttpRewriter> rewriter, kj::HttpHeaderTable& headerTable,
                      kj::Timer& timer, kj::EntropySource& entropySource,
                      capnp::ByteStreamFactory& byteStreamFactory,
                      capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
      : addr(kj::mv(addrParam)),
        inner(kj::newHttpClient(timer, headerTable, *addr, {
          .entropySource = entropySource,
          .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION
        })),
        serviceAdapter(kj::newHttpService(*inner)),
        rewriter(kj::mv(rewriter)),
        headerTable(headerTable),
        byteStreamFactory(byteStreamFactory),
        httpOverCapnpFactory(httpOverCapnpFactory),
        waitUntilTasks(*this) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return kj::heap<WorkerInterfaceImpl>(*this, kj::mv(metadata));
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    return handlerName == "fetch"_kj || handlerName == "connect"_kj;
  }

private:
  kj::Own<kj::NetworkAddress> addr;

  kj::Own<kj::HttpClient> inner;
  kj::Own<kj::HttpService> serviceAdapter;

  kj::Own<HttpRewriter> rewriter;

  kj::HttpHeaderTable& headerTable;
  capnp::ByteStreamFactory& byteStreamFactory;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  kj::TaskSet waitUntilTasks;

  void taskFailed(kj::Exception&& exception) override {
    LOG_EXCEPTION("externalServiceWaitUntilTasks", exception);
  }

  struct CapnpClient {
    kj::Own<kj::AsyncIoStream> connection;
    capnp::TwoPartyClient rpcSystem;

    CapnpClient(kj::Own<kj::AsyncIoStream> connectionParam)
        : connection(kj::mv(connectionParam)), rpcSystem(*connection) {}
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
    kj::StringPtr host = KJ_UNWRAP_OR(rewriter->getCapnpConnectHost(), {
      return JSG_KJ_EXCEPTION(FAILED, Error, "This ExternalServer not configured for RPC.");
    });

    auto req = client.connect(host, kj::HttpHeaders(headerTable), {});
    auto& c = capnpClient.emplace(kj::mv(req.connection));

    // Arrange that when the connection is lost, we'll null out `capnpClient`. This ensures that
    // on the next event, we'll attempt to reconnect.
    //
    // TODO(perf): Time out idle connections?
    clearCapnpClientTask = c.rpcSystem.onDisconnect()
        .attach(kj::defer([this]() { capnpClient = kj::none; }))
        .eagerlyEvaluate(nullptr);

    return c.rpcSystem.bootstrap().castAs<rpc::WorkerdBootstrap>();
  }

  class WorkerInterfaceImpl final: public WorkerInterface, private kj::HttpService::Response {
  public:
    WorkerInterfaceImpl(ExternalHttpService& parent, IoChannelFactory::SubrequestMetadata metadata)
        : parent(parent), metadata(kj::mv(metadata)) {}

    kj::Promise<void> request(
        kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
        kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override {
      TRACE_EVENT("workerd", "ExternalHttpServer::request()");
      KJ_REQUIRE(wrappedResponse == kj::none, "object should only receive one request");
      wrappedResponse = response;
      if (parent.rewriter->needsRewriteRequest()) {
        auto rewrite = parent.rewriter->rewriteOutgoingRequest(url, headers, metadata.cfBlobJson);
        return parent.serviceAdapter->request(method, url, *rewrite.headers, requestBody, *this)
            .attach(kj::mv(rewrite));
      } else {
        return parent.serviceAdapter->request(method, url, headers, requestBody, *this);
      }
    }

    kj::Promise<void> connect(
        kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
        ConnectResponse& tunnel, kj::HttpConnectSettings settings) override {
      TRACE_EVENT("workerd", "ExternalHttpServer::connect()");
      return parent.serviceAdapter->connect(host, headers, connection, tunnel, kj::mv(settings));
    }

    void prewarm(kj::StringPtr url) override {}
    kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
      throwUnsupported();
    }
    kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
      throwUnsupported();
    }

    kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
      // We'll use capnp RPC for custom events.
      auto bootstrap = parent.getOutgoingCapnp(*parent.inner);
      auto dispatcher =
          bootstrap.startEventRequest(capnp::MessageSize {4, 0}).send().getDispatcher();
      return event->sendRpc(parent.httpOverCapnpFactory, parent.byteStreamFactory,
                            parent.waitUntilTasks, kj::mv(dispatcher))
          .attach(kj::mv(event));
    }

  private:
    ExternalHttpService& parent;
    IoChannelFactory::SubrequestMetadata metadata;
    kj::Maybe<kj::HttpService::Response&> wrappedResponse;

    [[noreturn]] void throwUnsupported() {
      JSG_FAIL_REQUIRE(Error, "External HTTP servers don't support this event type.");
    }

    kj::Own<kj::AsyncOutputStream> send(
        uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize) override {
      TRACE_EVENT("workerd", "ExternalHttpService::send()", "status", statusCode);
      auto& response = KJ_ASSERT_NONNULL(wrappedResponse);
      if (parent.rewriter->needsRewriteResponse()) {
        auto rewrite = headers.cloneShallow();
        parent.rewriter->rewriteResponse(rewrite);
        return response.send(statusCode, statusText, rewrite, expectedBodySize);
      } else {
        return response.send(statusCode, statusText, headers, expectedBodySize);
      }
    }

    kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
      TRACE_EVENT("workerd", "ExternalHttpService::acceptWebSocket()");
      auto& response = KJ_ASSERT_NONNULL(wrappedResponse);
      if (parent.rewriter->needsRewriteResponse()) {
        auto rewrite = headers.cloneShallow();
        parent.rewriter->rewriteResponse(rewrite);
        return response.acceptWebSocket(rewrite);
      } else {
        return response.acceptWebSocket(headers);
      }
    }
  };
};

kj::Own<Server::Service> Server::makeExternalService(
    kj::StringPtr name, config::ExternalServer::Reader conf,
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
    reportConfigError(kj::str(
        "External service \"", name, "\" has no address in the config, so must be specified "
        "on the command line with `--external-addr`."));
    return makeInvalidConfigService();
  }

  switch (conf.which()) {
    case config::ExternalServer::HTTP: {
      // We have to construct the rewriter upfront before waiting on any promises, since the
      // HeaderTable::Builder is only available synchronously.
      auto rewriter = kj::heap<HttpRewriter>(conf.getHttp(), headerTableBuilder);
      auto addr = kj::heap<PromisedNetworkAddress>(network.parseAddress(addrStr, 80));
      return kj::heap<ExternalHttpService>(
          kj::mv(addr), kj::mv(rewriter), headerTableBuilder.getFutureTable(),
          timer, entropySource, globalContext->byteStreamFactory,
          globalContext->httpOverCapnpFactory);
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
      return kj::heap<ExternalHttpService>(
          kj::mv(addr), kj::mv(rewriter), headerTableBuilder.getFutureTable(),
          timer, entropySource, globalContext->byteStreamFactory,
          globalContext->httpOverCapnpFactory);
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
      return kj::heap<ExternalTcpService>(kj::mv(addr));
    }
  }
  reportConfigError(kj::str(
      "External service named \"", name, "\" has unrecognized protocol. Was the config "
      "compiled with a newer version of the schema?"));
  return makeInvalidConfigService();
}

// Service used when the service is configured as network service.
class Server::NetworkService final: public Service, private WorkerInterface {
public:
  NetworkService(kj::HttpHeaderTable& headerTable,
                 kj::Timer& timer, kj::EntropySource& entropySource,
                 kj::Own<kj::Network> networkParam,
                 kj::Maybe<kj::Own<kj::Network>> tlsNetworkParam,
                 kj::Maybe<kj::SecureNetworkWrapper&> tlsContext)
      : network(kj::mv(networkParam)), tlsNetwork(kj::mv(tlsNetworkParam)),
        inner(kj::newHttpClient(timer, headerTable, *network, tlsNetwork, {
          .entropySource = entropySource,
          .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION,
          .tlsContext = tlsContext
        })),
        serviceAdapter(kj::newHttpService(*inner)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return { this, kj::NullDisposer::instance };
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    return handlerName == "fetch"_kj || handlerName == "connect"_kj;
  }

private:
  kj::Own<kj::Network> network;
  kj::Maybe<kj::Own<kj::Network>> tlsNetwork;
  kj::Own<kj::HttpClient> inner;
  kj::Own<kj::HttpService> serviceAdapter;

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override {
    TRACE_EVENT("workerd", "NetworkService::request()");
    return serviceAdapter->request(method, url, headers, requestBody, response);
  }

  kj::Promise<void> connect(
      kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
      ConnectResponse& tunnel, kj::HttpConnectSettings settings) override {
    TRACE_EVENT("workerd", "NetworkService::connect()");
    // This code is hit when the global `connect` function is called in a JS worker script.
    // It represents a proxy-less TCP connection, which means we can simply defer the handling of
    // the connection to the service adapter (likely NetworkHttpClient). Its behaviour will be to
    // connect directly to the host over TCP.
    return serviceAdapter->connect(host, headers, connection, tunnel, kj::mv(settings));
  }

  void prewarm(kj::StringPtr url) override {}
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    throwUnsupported();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    throwUnsupported();
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    throwUnsupported();
  }

  [[noreturn]] void throwUnsupported() {
    JSG_FAIL_REQUIRE(Error, "External HTTP servers don't support this event type.");
  }
};

kj::Own<Server::Service> Server::makeNetworkService(config::Network::Reader conf) {
  TRACE_EVENT("workerd", "Server::makeNetworkService()");
  auto restrictedNetwork = network.restrictPeers(
      KJ_MAP(a, conf.getAllow()) -> kj::StringPtr { return a; },
      KJ_MAP(a, conf.getDeny() ) -> kj::StringPtr { return a; });

  kj::Maybe<kj::Own<kj::Network>> tlsNetwork;
  kj::Maybe<kj::SecureNetworkWrapper&> tlsContext;
  if (conf.hasTlsOptions()) {
    auto ownedTlsContext = makeTlsContext(conf.getTlsOptions());
    tlsContext = ownedTlsContext;
    tlsNetwork = ownedTlsContext->wrapNetwork(*restrictedNetwork).attach(kj::mv(ownedTlsContext));
  }

  return kj::heap<NetworkService>(globalContext->headerTable, timer, entropySource,
                                  kj::mv(restrictedNetwork), kj::mv(tlsNetwork), tlsContext);
}

// Service used when the service is configured as disk directory service.
class Server::DiskDirectoryService final: public Service, private WorkerInterface {
public:
  DiskDirectoryService(config::DiskDirectory::Reader conf,
                       kj::Own<const kj::Directory> dir,
                       kj::HttpHeaderTable::Builder& headerTableBuilder)
      : writable(*dir), readable(kj::mv(dir)), headerTable(headerTableBuilder.getFutureTable()),
        hLastModified(headerTableBuilder.add("Last-Modified")),
        allowDotfiles(conf.getAllowDotfiles()) {}
  DiskDirectoryService(config::DiskDirectory::Reader conf,
                       kj::Own<const kj::ReadableDirectory> dir,
                       kj::HttpHeaderTable::Builder& headerTableBuilder)
      : readable(kj::mv(dir)), headerTable(headerTableBuilder.getFutureTable()),
        hLastModified(headerTableBuilder.add("Last-Modified")),
        allowDotfiles(conf.getAllowDotfiles()) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return { this, kj::NullDisposer::instance };
  }

  kj::Maybe<const kj::Directory&> getWritable() { return writable; }

  bool hasHandler(kj::StringPtr handlerName) override {
    return handlerName == "fetch"_kj;
  }

private:
  kj::Maybe<const kj::Directory&> writable;
  kj::Own<const kj::ReadableDirectory> readable;
  kj::HttpHeaderTable& headerTable;
  kj::HttpHeaderId hLastModified;
  bool allowDotfiles;

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr urlStr, const kj::HttpHeaders& requestHeaders,
      kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override {
    TRACE_EVENT("workerd", "DiskDirectoryService::request()", "url", urlStr.cStr());
    auto url = kj::Url::parse(urlStr);

    bool blockedPath = false;
    kj::Path path = nullptr;
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      path = kj::Path(url.path.releaseAsArray());
    })) {
      (void)exception; // squash compiler warning about unused var
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

      auto file = KJ_UNWRAP_OR(readable->tryOpenFile(path), {
        co_return co_await response.sendError(404, "Not Found", headerTable);
      });

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
          //   HTTP API, then the header shoud be filled in automatically. Unclear if this is safe
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
                case kj::FsNode::Type::FILE:             type = "file"           ; break;
                case kj::FsNode::Type::DIRECTORY:        type = "directory"      ; break;
                case kj::FsNode::Type::SYMLINK:          type = "symlink"        ; break;
                case kj::FsNode::Type::BLOCK_DEVICE:     type = "blockDevice"    ; break;
                case kj::FsNode::Type::CHARACTER_DEVICE: type = "characterDevice"; break;
                case kj::FsNode::Type::NAMED_PIPE:       type = "namedPipe"      ; break;
                case kj::FsNode::Type::SOCKET:           type = "socket"         ; break;
                case kj::FsNode::Type::OTHER:            type = "other"          ; break;
              }

              jsonEntries.add(kj::str(
                  "{\"name\":\"", escapeJsonString(entry.name), "\","
                  "\"type\":\"", type, "\"}"));
            };

            auto content = kj::str('[', kj::strArray(jsonEntries, ","), ']');

            co_return co_await out->write(content.begin(), content.size());
          }
        }
        default:
          co_return co_await response.sendError(406, "Not Acceptable", headerTable);
      }
    } else if (method == kj::HttpMethod::PUT) {
      auto& w = KJ_UNWRAP_OR(writable, {
        co_return co_await response.sendError(405, "Method Not Allowed", headerTable);
      });

      if (blockedPath || path.size() == 0) {
        co_return co_await response.sendError(403, "Unauthorized", headerTable);
      }

      auto replacer = w.replaceFile(path,
          kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
      auto stream = kj::heap<kj::FileOutputStream>(replacer->get());

      co_await requestBody.pumpTo(*stream);

      replacer->commit();
      kj::HttpHeaders headers(headerTable);
      response.send(204, "No Content", headers);
      co_return;
    } else if (method == kj::HttpMethod::DELETE) {
      auto& w = KJ_UNWRAP_OR(writable, {
        co_return co_await response.sendError(405, "Method Not Allowed", headerTable);
      });

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

  kj::Promise<void> connect(kj::StringPtr host, const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection, kj::HttpService::ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    throwUnsupported();
  }
  void prewarm(kj::StringPtr url) override {}
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    throwUnsupported();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    throwUnsupported();
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    throwUnsupported();
  }

  [[noreturn]] void throwUnsupported() {
    JSG_FAIL_REQUIRE(Error, "Disk directory services don't support this event type.");
  }
};

kj::Own<Server::Service> Server::makeDiskDirectoryService(
    kj::StringPtr name, config::DiskDirectory::Reader conf,
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
    reportConfigError(kj::str(
        "Directory \"", name, "\" has no path in the config, so must be specified on the "
        "command line with `--directory-path`."));
    return makeInvalidConfigService();
  }

  auto path = fs.getCurrentPath().evalNative(pathStr);

  if (conf.getWritable()) {
    auto openDir = KJ_UNWRAP_OR(fs.getRoot().tryOpenSubdir(kj::mv(path), kj::WriteMode::MODIFY), {
      reportConfigError(kj::str(
          "Directory named \"", name, "\" not found: ", pathStr));
      return makeInvalidConfigService();
    });

    return kj::heap<DiskDirectoryService>(conf, kj::mv(openDir), headerTableBuilder);
  } else {
    auto openDir = KJ_UNWRAP_OR(fs.getRoot().tryOpenSubdir(kj::mv(path)), {
      reportConfigError(kj::str(
          "Directory named \"", name, "\" not found: ", pathStr));
      return makeInvalidConfigService();
    });

    return kj::heap<DiskDirectoryService>(conf, kj::mv(openDir), headerTableBuilder);
  }
}

// =======================================================================================

// This class exists to update the InspectorService's table of isolates when a config
// has multiple services. The InspectorService exists on the stack of it's own thread and
// initializes state that is bound to the thread, e.g. a http server and an event loop.
// This class provides a small thread-safe interface to the InspectorService so <name>:<isolate>
// mappings can be added after the InspectorService has started.
//
// The CloudFlare devtools only show the first service in workerd configuration. This service
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
  InspectorService(
      kj::Timer& timer,
      kj::HttpHeaderTable::Builder& headerTableBuilder,
      InspectorServiceIsolateRegistrar& registrar)
      : timer(timer),
        headerTable(headerTableBuilder.getFutureTable()),
        server(timer, headerTable, *this, kj::HttpServerSettings {
          .errorHandler = *this
        }),
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
    KJ_LOG(ERROR, kj::str("Uncaught exception: ", exception));
    KJ_IF_SOME(r, response) {
      co_return co_await r.sendError(500, "Internal Server Error", headerTable);
    }
  }

  kj::Promise<void> request(
      kj::HttpMethod method,
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
              co_return co_await ref->attachInspector(timer, timerOffset, *webSocket);
            } catch (...) {
              auto exception = kj::getCaughtExceptionAsKj();
              if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
                // This likely just means that the inspector client was closed.
                // Nothing to do here but move along.
                KJ_LOG(INFO, kj::str("Inspector client detached [", id, "]"));
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
      co_return co_await out->write(content.begin(), content.size());
    } else if (url.endsWith("/json") ||
               url.endsWith("/json/list") ||
               url.endsWith("/json/list?for_tab")) {
      responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());

      auto baseWsUrl = KJ_UNWRAP_OR(headers.get(kj::HttpHeaderId::HOST), {
        co_return co_await response.sendError(400, "Bad Request", responseHeaders);
      });

      kj::Vector<kj::String> entries(isolates.size());
      kj::Vector<kj::String> toRemove;
      for (auto& entry : isolates) {
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
          (void)ref; // squash compiler warning about unused ref
          kj::Vector<kj::String> fields(9);
          fields.add(kj::str("\"id\":\"", entry.key ,"\""));
          fields.add(kj::str("\"title\":\"workerd: worker ", entry.key ,"\""));
          fields.add(kj::str("\"type\":\"node\""));
          fields.add(kj::str("\"description\":\"workerd worker\""));
          fields.add(kj::str("\"webSocketDebuggerUrl\":\"ws://",
                              baseWsUrl ,"/", entry.key ,"\""));
          fields.add(kj::str("\"devtoolsFrontendUrl\":\"devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=", baseWsUrl ,"/\""));
          fields.add(kj::str("\"devtoolsFrontendUrlCompat\":\"devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=", baseWsUrl ,"/\""));
          fields.add(kj::str("\"faviconUrl\":\"https://workers.cloudflare.com/favicon.ico\""));
          fields.add(kj::str("\"url\":\"https://workers.dev\""));
          entries.add(kj::str('{', kj::strArray(fields, ",") ,'}'));
        } else {
          // If we're not able to get a reference to the isolate here, it's
          // been cleaned up and we should remove it from the list. We do this
          // after iterating to make sure we don't invalidate the iterator.
          toRemove.add(kj::str(entry.key));
        }
      }
      // Clean up if necessary
      for (auto& key : toRemove) {
        isolates.erase(key);
      }

      auto content = kj::str('[', kj::strArray(entries, ","), ']');

      auto out = response.send(200, "OK", responseHeaders, content.size());
      co_return co_await out->write(content.begin(), content.size()).attach(kj::mv(content),
                                    kj::mv(out));
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

void Server::InspectorServiceIsolateRegistrar::registerIsolate(kj::StringPtr name,
                                                               Worker::Isolate* isolate) {
  auto lockedInspectorService = this->inspectorService.lockExclusive();
  if (lockedInspectorService != nullptr) {
    auto is = const_cast<InspectorService*>(*lockedInspectorService);
    is->registerIsolate(name, isolate);
  }
}

// =======================================================================================

class Server::WorkerService final: public Service, private kj::TaskSet::ErrorHandler,
                                   private IoChannelFactory, private TimerChannel,
                                   private LimitEnforcer {
public:
  class ActorNamespace;

  // I/O channels, delivered when link() is called.
  struct LinkedIoChannels {
    kj::Array<Service*> subrequest;
    kj::Array<kj::Maybe<ActorNamespace&>> actor;  // null = configuration error
    kj::Maybe<Service&> cache;
    kj::Maybe<kj::Own<SqliteDatabase::Vfs>> actorStorage;
    AlarmScheduler& alarmScheduler;
  };
  using LinkCallback = kj::Function<LinkedIoChannels(WorkerService&)>;
  using AbortActorsCallback = kj::Function<void()>;

  WorkerService(ThreadContext& threadContext, kj::Own<const Worker> worker,
                kj::Maybe<kj::HashSet<kj::String>> defaultEntrypointHandlers,
                kj::HashMap<kj::String, kj::HashSet<kj::String>> namedEntrypointsParam,
                const kj::HashMap<kj::String, ActorConfig>& actorClasses,
                LinkCallback linkCallback, AbortActorsCallback abortActorsCallback)
      : threadContext(threadContext),
        ioChannels(kj::mv(linkCallback)),
        worker(kj::mv(worker)),
        defaultEntrypointHandlers(kj::mv(defaultEntrypointHandlers)),
        waitUntilTasks(*this), abortActorsCallback(kj::mv(abortActorsCallback)) {

    namedEntrypoints.reserve(namedEntrypointsParam.size());
    for (auto& ep: namedEntrypointsParam) {
      kj::StringPtr epPtr = ep.key;
      namedEntrypoints.insert(kj::mv(ep.key), EntrypointService(*this, epPtr, kj::mv(ep.value)));
    }

    actorNamespaces.reserve(actorClasses.size());
    for (auto& entry: actorClasses) {
      auto ns = kj::heap<ActorNamespace>(*this, entry.key, entry.value, threadContext.getUnsafeTimer());
      actorNamespaces.insert(entry.key, kj::mv(ns));
    }
  }

  kj::Maybe<Service&> getEntrypoint(kj::StringPtr name) {
    return namedEntrypoints.find(name);
  }

  kj::Array<kj::StringPtr> getEntrypointNames() {
    return KJ_MAP(e, namedEntrypoints) -> kj::StringPtr { return e.key; };
  }

  void link() override {
    LinkCallback callback = kj::mv(KJ_REQUIRE_NONNULL(
        ioChannels.tryGet<LinkCallback>(), "already called link()"));
    ioChannels = callback(*this);
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

  kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata) override {
    return startRequest(kj::mv(metadata), kj::none);
  }

  bool hasHandler(kj::StringPtr handlerName) override {
    KJ_IF_SOME(h, defaultEntrypointHandlers) {
      return h.contains(handlerName);
    } else {
      return false;
    }
  }

  kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata, kj::Maybe<kj::StringPtr> entrypointName,
      kj::Maybe<kj::Own<Worker::Actor>> actor = kj::none) {
    TRACE_EVENT("workerd", "Server::WorkerService::startRequest()");
    return newWorkerEntrypoint(
        threadContext,
        kj::atomicAddRef(*worker),
        entrypointName,
        kj::mv(actor),
        kj::Own<LimitEnforcer>(this, kj::NullDisposer::instance),
        {},                        // ioContextDependency
        kj::Own<IoChannelFactory>(this, kj::NullDisposer::instance),
        kj::refcounted<RequestObserver>(),  // default observer makes no observations
        waitUntilTasks,
        true,                      // tunnelExceptions
        kj::none,                  // workerTracer
        kj::mv(metadata.cfBlobJson));
  }

  class ActorNamespace final {
  public:
    ActorNamespace(WorkerService& service,kj::StringPtr className, const ActorConfig& config,
        kj::Timer& timer)
        : service(service),
          className(className),
          config(config),
          timer(timer) {}

    const ActorConfig& getConfig() { return config; }

    kj::Own<WorkerInterface> getActor(Worker::Actor::Id id,
        IoChannelFactory::SubrequestMetadata metadata) {
      kj::String idStr;
      KJ_SWITCH_ONEOF(id) {
        KJ_CASE_ONEOF(obj, kj::Own<ActorIdFactory::ActorId>) {
          KJ_REQUIRE(config.is<Durable>());
          idStr = obj->toString();
        }
        KJ_CASE_ONEOF(str, kj::String) {
          KJ_REQUIRE(config.is<Ephemeral>());
          idStr = kj::str(str);
        }
      }

      return getActor(kj::mv(idStr), kj::mv(metadata));
    }

    kj::Own<WorkerInterface> getActor(kj::String id,
        IoChannelFactory::SubrequestMetadata metadata) {
      return newPromisedWorkerInterface(service.waitUntilTasks,
          getActorThenStartRequest(kj::mv(id), kj::mv(metadata)));
    }

    kj::Own<IoChannelFactory::ActorChannel> getActorChannel(Worker::Actor::Id id) {
      return kj::heap<ActorChannelImpl>(*this, kj::mv(id));
    }

    // Forward declaration.
    class ActorContainerRef;

    // ActorContainer mostly serves as a wrapper around Worker::Actor.
    // We use it to associate a HibernationManager with the Worker::Actor, since the
    // Worker::Actor can be destroyed during periods of prolonged inactivity.
    //
    // We use a RequestTracker to track strong references to this ActorContainer's Worker::Actor.
    // Once there are no Worker::Actor's left (excluding our own), `inactive()` is triggered and we
    // initiate the eviction of the Durable Object. If no requests arrive in the next 10 seconds,
    // the DO is evicted, otherwise we cancel the eviction task.
    class ActorContainer final: public RequestTracker::Hooks {
    public:
      ActorContainer(kj::StringPtr key, ActorNamespace& parent, kj::Timer& timer)
          : key(key),
            tracker(kj::refcounted<RequestTracker>(*this)),
            parent(parent),
            timer(timer),
            lastAccess(timer.now()) {}

      ~ActorContainer() noexcept(false) {
        // Shutdown the tracker so we don't use active/inactive hooks anymore.
        tracker->shutdown();

        KJ_IF_SOME(a, actor) {
          // Unknown broken reason.
          auto reason = 0;
          a->shutdown(reason);
        }

        KJ_IF_SOME(ref, containerRef) {
          // We're being destroyed before the ActorContainerRef, probably because the actor broke.
          // Let's drop the ActorContainerRef's reference to us to prevent another eviction attempt.
          ref.container = kj::none;
        }

        // Don't erase the onBrokenTask if it is the reason we are being destroyed.
        if (!onBrokenTriggered) {
          parent.onBrokenTasks.erase(key);
        }
        // We need to make sure we're removed from the actors map.
        parent.actors.erase(key);
      }

      void active() override {
        // We're handling a new request, cancel the eviction promise.
        shutdownTask = kj::none;
      }

      void inactive() override {
        // Durable objects are evictable by default.
        bool isEvictable = true;
        KJ_SWITCH_ONEOF(parent.config) {
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
          shutdownTask = handleShutdown().eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
        }
      }

      // Processes the eviction of the Durable Object and hibernates active websockets.
      kj::Promise<void> handleShutdown() {
        // After 10 seconds of inactivity, we destroy the Worker::Actor and hibernate any active
        // JS WebSockets.
        // TODO(someday): We could make this timeout configurable to make testing less burdensome.
        co_await timer.afterDelay(10 * kj::SECONDS);
        KJ_IF_SOME(onBroken, parent.onBrokenTasks.findEntry(getKey())) {
          // Cancel the onBroken promise, since we're about to destroy the actor anyways and don't
          // want to trigger it.
          parent.onBrokenTasks.erase(onBroken);
        }
        KJ_IF_SOME(a, actor) {
          if (a->isShared()) {
            // Our ActiveRequest refcounting has broken somewhere. This is likely because we're
            // `addRef`-ing an actor that has had an ActiveRequest attached to its kj::Own (in other
            // words, the ActiveRequest count is less than it should be).
            //
            // Rather than dropping our actor and possibly ending up with split-brain,
            // we should opt out of the deferred proxy optimization and log the error to Sentry.
            KJ_LOG(ERROR,
                "Detected internal bug in hibernation: Durable Object has strong references "\
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
            workerStrongRef->runInLockScope(asyncLock, [&](Worker::Lock& lock) {
              m->hibernateWebSockets(lock);
            });
          }
          a->shutdown(0, KJ_EXCEPTION(DISCONNECTED,
              "broken.dropped; Actor freed due to inactivity"));
        }
        // Destory the last strong Worker::Actor reference.
        actor = kj::none;
      }

      kj::StringPtr getKey() { return key; }
      RequestTracker& getTracker() { return *tracker; }
      kj::Maybe<kj::Own<Worker::Actor::HibernationManager>> tryGetManagerRef() {
        return manager.map([&](kj::Own<Worker::Actor::HibernationManager>& m) {
          return kj::addRef(*m);
        });
      }
      void updateAccessTime() { lastAccess = timer.now(); }
      kj::TimePoint getLastAccess() { return lastAccess; }

      bool hasClients() { return containerRef != kj::none; }
      kj::Maybe<ActorContainerRef&> getContainerRef() { return containerRef; }

      // `onBrokenTriggered` indicates the actor has been broken.
      void setOnBroken() { onBrokenTriggered = true; }

      // The actor is constructed after the ActorContainer so it starts off empty.
      kj::Maybe<kj::Own<Worker::Actor>> actor;
    private:
      kj::StringPtr key;
      kj::Own<RequestTracker> tracker;
      ActorNamespace& parent;
      kj::Timer& timer;
      kj::TimePoint lastAccess;
      kj::Maybe<kj::Own<Worker::Actor::HibernationManager>> manager;
      kj::Maybe<kj::Promise<void>> shutdownTask;
      bool onBrokenTriggered = false;

      // Non-empty if at least one client has a reference to this actor.
      // If no clients are connected, we may be evicted by `cleanupLoop`.
      kj::Maybe<ActorContainerRef&> containerRef;
      friend class ActorContainerRef;
    };

    // This class tracks clients that a have reference to the given actor.
    // Upon destruction, we update the lastAccess time for the actor and
    // `ActorContainer::hasClients()` starts returning false. After 70 seconds, the cleanupLoop
    // will remove the `ActorContainer` from `actors`.
    class ActorContainerRef: public kj::Refcounted {
    public:
      ActorContainerRef(ActorContainer& container): container(container) {
        // Link this ref to the actual ActorContainer.
        container.containerRef = *this;
      }
      ~ActorContainerRef() noexcept(false) {
        KJ_IF_SOME(ref, container) {
          ref.updateAccessTime();
          ref.containerRef = kj::none;
        }
      }

      kj::Own<ActorContainerRef> addRef() {
        return kj::addRef(*this);
      }

    private:
      // This is a maybe because the ActorContainer could be destroyed before ActorContainerRef
      // if the actor is broken.
      kj::Maybe<ActorContainer&> container;
      friend class ActorContainer;
    };

    void abortAll() {
      actors.clear();
    }

  private:
    WorkerService& service;
    kj::StringPtr className;
    const ActorConfig& config;
    // If the actor is broken, we remove it from the map. However, if it's just evicted due to
    // inactivity, we keep the ActorContainer in the map but drop the Own<Worker::Actor>. When a new
    // request comes in, we recreate the Own<Worker::Actor>.
    kj::HashMap<kj::String, kj::Own<ActorContainer>> actors;
    kj::HashMap<kj::String, kj::Maybe<kj::Promise<void>>> onBrokenTasks;
    kj::Maybe<kj::Promise<void>> cleanupTask;
    kj::Timer& timer;

    // An owned actor and an ActorContainerRef
    // used to track the client that requested it.
    struct GetActorResult {
      kj::Own<Worker::Actor> actor;
      kj::Own<ActorContainerRef> ref;
    };

    kj::Promise<kj::Own<WorkerInterface>> getActorThenStartRequest(
        kj::String id,
        IoChannelFactory::SubrequestMetadata metadata) {
      auto [actor, refTracker] = co_await getActorImpl(kj::mv(id));

      if (cleanupTask == kj::none) {
        // Need to start the cleanup loop.
        cleanupTask = cleanupLoop();
      }

      co_return service.startRequest(kj::mv(metadata), className, kj::mv(actor))
          .attach(kj::mv(refTracker));
    }

    // Removes actors from `actors` after 70 seconds of last access.
    kj::Promise<void> cleanupLoop() {
      constexpr auto EXPIRATION = 70 * kj::SECONDS;

      while (true) {
        auto now = timer.now();
        actors.eraseAll([&](auto&, kj::Own<ActorContainer>& entry) {

          // Durable Objects are evictable by default.
          bool isEvictable = true;
          KJ_SWITCH_ONEOF(config) {
            KJ_CASE_ONEOF(c, Durable) {
              isEvictable = c.isEvictable;
            }
            KJ_CASE_ONEOF(c, Ephemeral) {
              isEvictable = c.isEvictable;
            }
          }
          if (entry->hasClients() || !isEvictable) {
            // We are still using the actor so we cannot remove it, or this actor cannot be evicted.
            return false;
          }

          return (now - entry->getLastAccess()) > EXPIRATION;
        });

        co_await timer.afterDelay(EXPIRATION).eagerlyEvaluate(nullptr);
      }
    }

    // Implements actor loopback, which is used by websocket hibernation to deliver events to the
    // actor from the websocket's read loop.
    class Loopback : public Worker::Actor::Loopback, public kj::Refcounted {
    public:
      Loopback(ActorNamespace& ns, kj::String id) : ns(ns), id(kj::mv(id)) {}

      kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata metadata) {
        return ns.getActor(kj::str(id), kj::mv(metadata));
      }

      kj::Own<Worker::Actor::Loopback> addRef() {
        return kj::addRef(*this);
      }

    private:
      ActorNamespace& ns;
      kj::String id;
    };

    class ActorSqliteHooks final : public ActorSqlite::Hooks {
    public:
      ActorSqliteHooks(AlarmScheduler& alarmScheduler, ActorKey actor)
          : alarmScheduler(alarmScheduler), actor(actor) {}

      kj::Promise<kj::Maybe<kj::Date>> getAlarm() override {
        return alarmScheduler.getAlarm(actor);
      }

      kj::Promise<void> setAlarm(kj::Maybe<kj::Date> newAlarmTime) override {
        KJ_IF_SOME(scheduledTime, newAlarmTime) {
          alarmScheduler.setAlarm(actor, scheduledTime);
        } else {
          alarmScheduler.deleteAlarm(actor);
        }
        return kj::READY_NOW;
      }

      // No-op -- armAlarmHandler() is normally used to schedule a delete after the alarm runs.
      // But since alarm read/write operations happen on the same thread as the scheduler in
      // workerd, we can just handle the delete in the scheduler instead.
      kj::Maybe<kj::Own<void>> armAlarmHandler(kj::Date, bool) override {
        // We return this weird kj::Own<void> to `this` since just doing kj::Own<void>() creates an
        // empty maybe.
        return kj::Own<void>(this, kj::NullDisposer::instance);
      }

      void cancelDeferredAlarmDeletion() override {}

    private:
      AlarmScheduler& alarmScheduler;
      ActorKey actor;
    };

    kj::Promise<GetActorResult> getActorImpl(kj::String id) {
      // `getActor()` is often called with the calling isolate's lock held. We need to drop that
      // lock and take a lock on the target isolate before constructing the actor. Even if these
      // are the same isolate (as is commonly the case), we really don't want to do this stuff
      // synchronously, so this has the effect of pushing off to a later turn of the event loop.
      return service.worker->takeAsyncLockWithoutRequest(nullptr).then(
          [this, id = kj::mv(id)] (Worker::AsyncLock asyncLock) mutable -> GetActorResult {
        kj::StringPtr idPtr = id;
        auto& actorContainer = actors.findOrCreate(id, [&]() mutable {
          auto container = kj::heap<ActorContainer>(idPtr, *this, timer);

          return kj::HashMap<kj::String, kj::Own<ActorContainer>>::Entry {
            kj::mv(id), kj::mv(container)
          };
        });

        // If we don't have an ActorContainerRef, we'll create one to track the client.
        auto actor = [&]() mutable {
          KJ_IF_SOME(a, actorContainer->actor) {
            // This actor was used recently and hasn't been evicted, let's reuse it.
            KJ_IF_SOME(ref, actorContainer->getContainerRef()) {
              return GetActorResult { .actor = a->addRef(), .ref = ref.addRef() };
            }
            // We have an actor, but all the clients dropped their reference to the DO so we need
            // make a new `ActorContainerRef`. Note that `hasClients()` will return true now,
            // preventing cleanupLoop from evicting us.
            return GetActorResult {
                .actor = a->addRef(),
                .ref = kj::refcounted<ActorContainerRef>(*actorContainer) };
          }
          // We don't have an actor so we need to create it.
          auto& channels = KJ_ASSERT_NONNULL(service.ioChannels.tryGet<LinkedIoChannels>());

          auto makeActorCache =
              [&](const ActorCache::SharedLru& sharedLru, OutputGate& outputGate,
                  ActorCache::Hooks& hooks) {
            return config.tryGet<Durable>()
                .map([&](const Durable& d) -> kj::Own<ActorCacheInterface> {
              KJ_IF_SOME(as, channels.actorStorage) {
                // The idPtr can end up being freed if the Actor gets hibernated so we need
                // to create a copy that is ensured to live as long as the ActorSqliteHooks
                // instance we're creating here.
                // TODO(cleanup): Is there a better way to handle the ActorKey in general here?
                auto idStr = kj::str(idPtr);
                auto sqliteHooks = kj::heap<ActorSqliteHooks>(channels.alarmScheduler, ActorKey{
                  .uniqueKey = d.uniqueKey, .actorId = idStr
                }).attach(kj::mv(idStr));

                auto db = kj::heap<SqliteDatabase>(*as,
                    kj::Path({d.uniqueKey, kj::str(idPtr, ".sqlite")}),
                    kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
                return kj::heap<ActorSqlite>(kj::mv(db), outputGate,
                    []() -> kj::Promise<void> { return kj::READY_NOW; },
                    *sqliteHooks).attach(kj::mv(sqliteHooks));
              } else {
                // Create an ActorCache backed by a fake, empty storage. Elsewhere, we configure
                // ActorCache never to flush, so this effectively creates in-memory storage.
                return kj::heap<ActorCache>(
                    kj::heap<EmptyReadOnlyActorStorageImpl>(), sharedLru, outputGate, hooks);
              }
            });
          };

          auto makeStorage = [](jsg::Lock& js, const Worker::Api& api,
                                ActorCacheInterface& actorCache)
                            -> jsg::Ref<api::DurableObjectStorage> {
            return jsg::alloc<api::DurableObjectStorage>(
                IoContext::current().addObject(actorCache));
          };

          TimerChannel& timerChannel = service;

          auto loopback = kj::refcounted<Loopback>(*this, kj::str(idPtr));

          return service.worker->runInLockScope(asyncLock, [&](Worker::Lock& lock) {
            // We define this event ID in the internal codebase, but to have WebSocket Hibernation
            // work for local development we need to pass an event type.
            static constexpr uint16_t hibernationEventTypeId = 8;

            actorContainer->actor.emplace(
                kj::refcounted<Worker::Actor>(
                    *service.worker, actorContainer->getTracker(), kj::str(idPtr), true,
                    kj::mv(makeActorCache), className, kj::mv(makeStorage), lock, kj::mv(loopback),
                    timerChannel, kj::refcounted<ActorObserver>(),
                    actorContainer->tryGetManagerRef(),
                    hibernationEventTypeId));

            // If the actor becomes broken, remove it from the map, so a new one will be created
            // next time.
            auto& actorRef = KJ_REQUIRE_NONNULL(actorContainer->actor);
            auto& entry = onBrokenTasks.findOrCreateEntry(actorContainer->getKey(), [&](){
              return decltype(onBrokenTasks)::Entry {
                kj::str(actorContainer->getKey()), kj::none
              };
            });
            entry.value = onActorBroken(actorRef->onBroken(), *actorContainer)
                .eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });

            // `hasClients()` will return true now, preventing cleanupLoop from evicting us.
            return GetActorResult {
                .actor = actorRef->addRef(),
                .ref = kj::refcounted<ActorContainerRef>(*actorContainer) };
          });
        }();

        return kj::mv(actor);
      });
    }

    kj::Promise<void> onActorBroken(kj::Promise<void> broken, ActorContainer& entryRef) {
      try {
        // It's possible for this to never resolve if the actor never breaks,
        // in which case the returned promise will just be canceled.
        co_await broken;
      } catch (...) {
        // We are intentionally ignoring any errors here. We just want to ensure
        // that the actor is removed if the onBroken promise is resolved or errors.
      }
      // Note that we remove the entire ActorContainer from the map -- this drops the
      // HibernationManager so any connected hibernatable websockets will be disconnected.
      entryRef.setOnBroken();
      auto key = kj::str(entryRef.getKey());
      actors.erase(key.asPtr());
    }
  };

private:
  class EntrypointService final: public Service {
  public:
    EntrypointService(WorkerService& worker, kj::StringPtr entrypoint,
                      kj::HashSet<kj::String> handlers)
        : worker(worker), entrypoint(entrypoint), handlers(kj::mv(handlers)) {}

    kj::Own<WorkerInterface> startRequest(
        IoChannelFactory::SubrequestMetadata metadata) override {
      return worker.startRequest(kj::mv(metadata), entrypoint);
    }

    bool hasHandler(kj::StringPtr handlerName) override {
      return handlers.contains(handlerName);
    }

  private:
    WorkerService& worker;
    kj::StringPtr entrypoint;
    kj::HashSet<kj::String> handlers;
  };

  ThreadContext& threadContext;

  // LinkedIoChannels owns the SqliteDatabase::Vfs, so make sure it is destroyed last.
  kj::OneOf<LinkCallback, LinkedIoChannels> ioChannels;

  kj::Own<const Worker> worker;
  kj::Maybe<kj::HashSet<kj::String>> defaultEntrypointHandlers;
  kj::HashMap<kj::String, EntrypointService> namedEntrypoints;
  kj::HashMap<kj::StringPtr, kj::Own<ActorNamespace>> actorNamespaces;
  kj::TaskSet waitUntilTasks;
  AbortActorsCallback abortActorsCallback;

  class ActorChannelImpl final: public IoChannelFactory::ActorChannel {
  public:
    ActorChannelImpl(ActorNamespace& ns, Worker::Actor::Id id)
        : ns(ns), id(kj::mv(id)) {}

    kj::Own<WorkerInterface> startRequest(
        IoChannelFactory::SubrequestMetadata metadata) override {
      return ns.getActor(Worker::Actor::cloneId(id), kj::mv(metadata));
    }

  private:
    ActorNamespace& ns;
    Worker::Actor::Id id;
  };

  // ---------------------------------------------------------------------------
  // implements kj::TaskSet::ErrorHandler

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, exception);
  }

  // ---------------------------------------------------------------------------
  // implements IoChannelFactory

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    auto& channels = KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(),
        "link() has not been called");

    KJ_REQUIRE(channel < channels.subrequest.size(), "invalid subrequest channel number");
    return channels.subrequest[channel]->startRequest(kj::mv(metadata));
  }

  capnp::Capability::Client getCapability(uint channel) override {
    KJ_FAIL_REQUIRE("no capability channels");
  }
  class CacheClientImpl final: public CacheClient {
  public:
    CacheClientImpl(Service& cacheService, kj::HttpHeaderId cacheNamespaceHeader)
        : cacheService(cacheService), cacheNamespaceHeader(cacheNamespaceHeader) {}

    kj::Own<kj::HttpClient> getDefault(kj::Maybe<kj::String> cfBlobJson,
                                       SpanParent parentSpan) override {

      return kj::heap<CacheHttpClientImpl>(
          cacheService, cacheNamespaceHeader, kj::none, kj::mv(cfBlobJson), kj::mv(parentSpan));
    }

    kj::Own<kj::HttpClient> getNamespace(kj::StringPtr cacheName,
                                         kj::Maybe<kj::String> cfBlobJson,
                                         SpanParent parentSpan) override {
      auto encodedName = kj::encodeUriComponent(cacheName);
      return kj::heap<CacheHttpClientImpl>(
          cacheService, cacheNamespaceHeader, kj::mv(encodedName), kj::mv(cfBlobJson),
          kj::mv(parentSpan));
    }

  private:
    Service& cacheService;
    kj::HttpHeaderId cacheNamespaceHeader;

  };

  class CacheHttpClientImpl final: public kj::HttpClient {
  public:
    CacheHttpClientImpl(Service& parent, kj::HttpHeaderId cacheNamespaceHeader,
                        kj::Maybe<kj::String> cacheName, kj::Maybe<kj::String> cfBlobJson,
                        SpanParent parentSpan)
        : client(asHttpClient(parent.startRequest({kj::mv(cfBlobJson), kj::mv(parentSpan)}))),
          cacheName(kj::mv(cacheName)),
          cacheNamespaceHeader(cacheNamespaceHeader) {}

    Request request(kj::HttpMethod method, kj::StringPtr url,
                    const kj::HttpHeaders &headers,
                    kj::Maybe<uint64_t> expectedBodySize = kj::none) override {

      return client->request(method, url, addCacheNameHeader(headers, cacheName),
                             expectedBodySize);
    }

  private:
    kj::Own<kj::HttpClient> client;
    kj::Maybe<kj::String> cacheName;
    kj::HttpHeaderId cacheNamespaceHeader;

    kj::HttpHeaders addCacheNameHeader(const kj::HttpHeaders& headers,
                                       kj::Maybe<kj::StringPtr> cacheName) {
      auto headersCopy = headers.cloneShallow();
      KJ_IF_SOME (name, cacheName) {
        headersCopy.set(cacheNamespaceHeader, name);
      }

      return headersCopy;
    }
  };

  kj::Own<CacheClient> getCache() override {
    auto& channels = KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(),
                                        "link() has not been called");
    auto& cache = JSG_REQUIRE_NONNULL(channels.cache, Error, "No Cache was configured");
    return kj::heap<CacheClientImpl>(cache, threadContext.getHeaderIds().cfCacheNamespace);
  }

  TimerChannel& getTimer() override {
    return *this;
  }

  kj::Promise<void> writeLogfwdr(uint channel,
      kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
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

    struct RefcountedWrapper: public kj::Refcounted {
      explicit RefcountedWrapper(kj::Own<kj::HttpClient> client): client(kj::mv(client)) {}
      kj::Own<kj::HttpClient> client;
    };
    auto rcClient = kj::refcounted<RefcountedWrapper>(kj::mv(client));
    auto request = attachToRequest(kj::mv(innerReq), kj::mv(rcClient));

    co_await request.body->write(requestJson.begin(), requestJson.size())
          .attach(kj::mv(requestJson), kj::mv(request.body));
    auto response = co_await request.response;

    KJ_REQUIRE(response.statusCode >= 200 && response.statusCode < 300, "writeLogfwdr request returned an error");
    co_await response.body->readAllBytes().attach(kj::mv(response.body)).ignoreResult();
    co_return;
  }

  kj::Own<ActorChannel> getGlobalActor(uint channel, const ActorIdFactory::ActorId& id,
      kj::Maybe<kj::String> locationHint, ActorGetMode mode, SpanParent parentSpan) override {
    JSG_REQUIRE(mode == ActorGetMode::GET_OR_CREATE, Error,
        "workerd only supports GET_OR_CREATE mode for getting actor stubs");
    auto& channels = KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(),
        "link() has not been called");

    KJ_REQUIRE(channel < channels.actor.size(), "invalid actor channel number");
    auto& ns = JSG_REQUIRE_NONNULL(channels.actor[channel], Error,
        "Actor namespace configuration was invalid.");
    KJ_REQUIRE(ns.getConfig().is<Durable>());  // should have been verified earlier
    return ns.getActorChannel(id.clone());
  }

  kj::Own<ActorChannel> getColoLocalActor(uint channel, kj::StringPtr id,
      SpanParent parentSpan) override {
    auto& channels = KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(),
        "link() has not been called");

    KJ_REQUIRE(channel < channels.actor.size(), "invalid actor channel number");
    auto& ns = JSG_REQUIRE_NONNULL(channels.actor[channel], Error,
        "Actor namespace configuration was invalid.");
    KJ_REQUIRE(ns.getConfig().is<Ephemeral>());  // should have been verified earlier
    return ns.getActorChannel(kj::str(id));
  }

  void abortAllActors() override {
    abortActorsCallback();
  }

  // ---------------------------------------------------------------------------
  // implements TimerChannel

  void syncTime() override {
    // Nothing to do
  }

  kj::Date now() override {
    return kj::systemPreciseCalendarClock().now();
  }

  kj::Promise<void> atTime(kj::Date when) override {
    return threadContext.getUnsafeTimer().afterDelay(when - now());
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration t) override {
    return threadContext.getUnsafeTimer().afterDelay(t);
  }

  // ---------------------------------------------------------------------------
  // implements LimitEnforcer
  //
  // No limits are enforced.

  kj::Own<void> enterJs(jsg::Lock& lock, IoContext& context) override { return {}; }
  void topUpActor() override {}
  void newSubrequest(bool isInHouse) override {}
  void newKvRequest(KvOpType op) override {}
  void newAnalyticsEngineRequest() override {}
  kj::Promise<void> limitDrain() override { return kj::NEVER_DONE; }
  kj::Promise<void> limitScheduled() override { return kj::NEVER_DONE; }
  kj::Duration getAlarmLimit() override { return 0 * kj::MILLISECONDS; }
  size_t getBufferingLimit() override { return kj::maxValue; }
  kj::Maybe<EventOutcome> getLimitsExceeded() override { return kj::none; }
  kj::Promise<void> onLimitsExceeded() override { return kj::NEVER_DONE; }
  void requireLimitsNotExceeded() override {}
  void reportMetrics(RequestObserver& requestMetrics) override {}

  // Unlike the other metrics, use the 5GB default limit here. Ideally this limit would be
  // configurable at runtime for better testing, but in this case going beyond the default limit
  // should rarely be needed.
  kj::Maybe<uint64_t> getCachePUTLimitMB() override { return kj::none; }
};

struct FutureSubrequestChannel {
  config::ServiceDesignator::Reader designator;
  kj::String errorContext;
};

struct FutureActorChannel {
  config::Worker::Binding::DurableObjectNamespaceDesignator::Reader designator;
  kj::String errorContext;
};

static kj::Maybe<WorkerdApi::Global> createBinding(
    kj::StringPtr workerName,
    config::Worker::Reader conf,
    config::Worker::Binding::Reader binding,
    Worker::ValidationErrorReporter& errorReporter,
    kj::Vector<FutureSubrequestChannel>& subrequestChannels,
    kj::Vector<FutureActorChannel>& actorChannels,
    kj::HashMap<kj::String, kj::HashMap<kj::String, Server::ActorConfig>>& actorConfigs,
    bool experimental) {
  // creates binding object or returns null and reports an error
  using Global = WorkerdApi::Global;
  kj::StringPtr bindingName = binding.getName();
  TRACE_EVENT("workerd", "Server::WorkerService::createBinding()",
              "name", workerName.cStr(),
              "binding", bindingName.cStr());
  auto makeGlobal = [&](auto&& value) {
    return Global{.name = kj::str(bindingName), .value = kj::mv(value)};
  };

  auto errorContext = kj::str("Worker \"", workerName , "\"'s binding \"", bindingName, "\"");

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
      return makeGlobal(Global::Json { kj::str(binding.getJson()) });

    case config::Worker::Binding::WASM_MODULE:
      if (conf.isServiceWorkerScript()) {
        // Already handled earlier.
      } else {
        errorReporter.addError(kj::str(
            errorContext, " is a Wasm binding, but Wasm bindings are not allowed in "
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
            errorReporter.addError(kj::str(
                "CryptoKey binding \"", binding.getName(), "\" contained invalid hex."));
          }
          keyGlobal.keyData = kj::Array<byte>(kj::mv(decoded));
          goto validFormat;
        }
        case config::Worker::Binding::CryptoKey::BASE64: {
          keyGlobal.format = kj::str("raw");
          auto decoded = kj::decodeBase64(keyConf.getBase64());
          if (decoded.hadErrors) {
            errorReporter.addError(kj::str(
                "CryptoKey binding \"", binding.getName(), "\" contained invalid base64."));
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
            errorReporter.addError(kj::str(
                "CryptoKey binding \"", binding.getName(), "\" contained wrong PEM type, "
                "expected \"PRIVATE KEY\" but got \"", pem.type, "\"."));
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
            errorReporter.addError(kj::str(
                "CryptoKey binding \"", binding.getName(), "\" contained wrong PEM type, "
                "expected \"PUBLIC KEY\" but got \"", pem.type, "\"."));
            return kj::none;
          }
          keyGlobal.keyData = kj::mv(pem.data);
          goto validFormat;
        }
        case config::Worker::Binding::CryptoKey::JWK:
          keyGlobal.format = kj::str("jwk");
          keyGlobal.keyData = Global::Json { kj::str(keyConf.getJwk()) };
          goto validFormat;
      }
      errorReporter.addError(kj::str(
          "Encountered unknown CryptoKey type for binding \"", binding.getName(),
          "\". Was the config compiled with a newer version of the schema?"));
      return kj::none;
    validFormat:

      auto algorithmConf = keyConf.getAlgorithm();
      switch (algorithmConf.which()) {
        case config::Worker::Binding::CryptoKey::Algorithm::NAME:
          keyGlobal.algorithm = Global::Json {
            kj::str('"', escapeJsonString(algorithmConf.getName()), '"')
          };
          goto validAlgorithm;
        case config::Worker::Binding::CryptoKey::Algorithm::JSON:
          keyGlobal.algorithm = Global::Json { kj::str(algorithmConf.getJson()) };
          goto validAlgorithm;
      }
      errorReporter.addError(kj::str(
          "Encountered unknown CryptoKey algorithm type for binding \"", binding.getName(),
          "\". Was the config compiled with a newer version of the schema?"));
      return kj::none;
    validAlgorithm:

      keyGlobal.extractable = keyConf.getExtractable();
      keyGlobal.usages = KJ_MAP(usage, keyConf.getUsages()) { return kj::str(usage); };

      return makeGlobal(kj::mv(keyGlobal));
      return kj::none;
    }

    case config::Worker::Binding::SERVICE: {
      uint channel = (uint)subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel {
        binding.getService(),
        kj::mv(errorContext)
      });
      return makeGlobal(Global::Fetcher {
        .channel = channel,
        .requiresHost = true,
        .isInHouse = false
      });
    }

    case config::Worker::Binding::DURABLE_OBJECT_NAMESPACE: {
      auto actorBinding = binding.getDurableObjectNamespace();
      const Server::ActorConfig* actorConfig;
      if (actorBinding.hasServiceName()) {
        auto& svcMap = KJ_UNWRAP_OR(actorConfigs.find(actorBinding.getServiceName()), {
          errorReporter.addError(kj::str(
              errorContext, " refers to a service \"", actorBinding.getServiceName(),
              "\", but no such service is defined."));
          return kj::none;
        });

        actorConfig = &KJ_UNWRAP_OR(svcMap.find(actorBinding.getClassName()), {
          errorReporter.addError(kj::str(
              errorContext, " refers to a Durable Object namespace named \"",
              actorBinding.getClassName(), "\" in service \"", actorBinding.getServiceName(),
              "\", but no such Durable Object namespace is defined by that service."));
          return kj::none;
        });
      } else {
          auto& localActorConfigs = KJ_ASSERT_NONNULL(actorConfigs.find(workerName));
          actorConfig = &KJ_UNWRAP_OR(localActorConfigs.find(actorBinding.getClassName()), {
          errorReporter.addError(kj::str(
              errorContext, " refers to a Durable Object namespace named \"",
              actorBinding.getClassName(), "\", but no such Durable Object namespace is defined "
              "by this Worker."));
          return kj::none;
        });
      }

      uint channel = (uint)actorChannels.size();
      actorChannels.add(FutureActorChannel{actorBinding, kj::mv(errorContext)});

      KJ_SWITCH_ONEOF(*actorConfig) {
        KJ_CASE_ONEOF(durable, Server::Durable) {
          return makeGlobal(Global::DurableActorNamespace {
            .actorChannel = channel,
            .uniqueKey = durable.uniqueKey
          });
        }
        KJ_CASE_ONEOF(_, Server::Ephemeral) {
          return makeGlobal(Global::EphemeralActorNamespace{.actorChannel = channel});
        }
      }

      return kj::none;
    }

    case config::Worker::Binding::KV_NAMESPACE: {
      uint channel = (uint)subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel {
        binding.getKvNamespace(),
        kj::mv(errorContext)
      });

      return makeGlobal(Global::KvNamespace{.subrequestChannel = channel});
    }

    case config::Worker::Binding::R2_BUCKET: {
      uint channel = (uint)subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel {
        binding.getR2Bucket(),
        kj::mv(errorContext)
      });
      return makeGlobal(Global::R2Bucket{.subrequestChannel = channel});
    }

    case config::Worker::Binding::R2_ADMIN: {
      uint channel = (uint)subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel {
        binding.getR2Admin(),
        kj::mv(errorContext)
      });
      return makeGlobal(Global::R2Admin{.subrequestChannel = channel});
    }

    case config::Worker::Binding::QUEUE: {
      uint channel = (uint)subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel {
        binding.getQueue(),
        kj::mv(errorContext)
      });

      return makeGlobal(Global::QueueBinding{.subrequestChannel = channel});
    }

    case config::Worker::Binding::WRAPPED: {
      auto wrapped = binding.getWrapped();
      kj::Vector<Global> innerGlobals;
      for (const auto& innerBinding: wrapped.getInnerBindings()) {
        KJ_IF_SOME(global, createBinding(workerName, conf, innerBinding,
            errorReporter, subrequestChannels, actorChannels, actorConfigs, experimental)) {
          innerGlobals.add(kj::mv(global));
        } else {
          // we've already communicated the error
          return kj::none;
        }
      }
      return makeGlobal(Global::Wrapped {
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
        return makeGlobal(Global::Json { kj::str("null") });
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

      uint channel = (uint)subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel {
        binding.getAnalyticsEngine(),
        kj::mv(errorContext)
      });

      return makeGlobal(Global::AnalyticsEngine{
        .subrequestChannel = channel,
        .dataset = kj::str(binding.getAnalyticsEngine().getName()),
        .version = 0,
      });
    }
    case config::Worker::Binding::HYPERDRIVE: {
      uint channel = (uint)subrequestChannels.size() + IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
      subrequestChannels.add(FutureSubrequestChannel {
        binding.getHyperdrive().getDesignator(),
        kj::mv(errorContext)
      });
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
      return makeGlobal(Global::UnsafeEval {});
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
        errorReporter.addError(kj::str("MemoryCache bindings must specify limits. Please "
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
  }
  errorReporter.addError(kj::str(
      errorContext, "has unrecognized type. Was the config compiled with a newer version of "
      "the schema?"));
}

uint startInspector(kj::StringPtr inspectorAddress, Server::InspectorServiceIsolateRegistrar& registrar);

void Server::abortAllActors() {
  for (auto& service: services) {
    if (WorkerService* worker = dynamic_cast<WorkerService*>(&*service.value)) {
      for (auto& [className, ns] : worker->getActorNamespaces()) {
        bool isEvictable = true;
        KJ_SWITCH_ONEOF(ns->getConfig()) {
          KJ_CASE_ONEOF(c, Durable) {
            isEvictable = c.isEvictable;
          }
          KJ_CASE_ONEOF(c, Ephemeral) {
            isEvictable = c.isEvictable;
          }
        }
        if (isEvictable) ns->abortAll();
      }
    }
  }
}

kj::Own<Server::Service> Server::makeWorker(kj::StringPtr name, config::Worker::Reader conf,
    capnp::List<config::Extension>::Reader extensions) {
  TRACE_EVENT("workerd", "Server::makeWorker()", "name", name.cStr());
  auto& localActorConfigs = KJ_ASSERT_NONNULL(actorConfigs.find(name));

  struct ErrorReporter: public Worker::ValidationErrorReporter {
    ErrorReporter(Server& server, kj::StringPtr name): server(server), name(name) {}

    Server& server;
    kj::StringPtr name;

    kj::HashMap<kj::String, kj::HashSet<kj::String>> namedEntrypoints;

    // The `HashSet`s are the set of exported handlers, like `fetch`, `test`, etc.
    kj::Maybe<kj::HashSet<kj::String>> defaultEntrypoint;

    void addError(kj::String error) override {
      server.reportConfigError(kj::str("service ", name, ": ", error));
    }

    void addHandler(kj::Maybe<kj::StringPtr> exportName, kj::StringPtr type) override {
      kj::HashSet<kj::String>* set;
      KJ_IF_SOME(e, exportName) {
        set = &namedEntrypoints.findOrCreate(e,
            [&]() -> decltype(namedEntrypoints)::Entry { return { kj::str(e), {} }; });
      } else {
        set = &defaultEntrypoint.emplace();
      }
      set->insert(kj::str(type));
    }
  };

  ErrorReporter errorReporter(*this, name);

  capnp::MallocMessageBuilder arena;
  // TODO(beta): Factor out FeatureFlags from WorkerBundle.
  auto featureFlags = arena.initRoot<CompatibilityFlags>();

  if (conf.hasCompatibilityDate()) {
    compileCompatibilityFlags(conf.getCompatibilityDate(), conf.getCompatibilityFlags(),
                              featureFlags, errorReporter, experimental,
                              CompatibilityDateValidation::CODE_VERSION);
  } else {
    errorReporter.addError(kj::str("Worker must specify compatibilityDate."));
  }

  // IsolateLimitEnforcer that enforces no limits.
  class NullIsolateLimitEnforcer final: public IsolateLimitEnforcer {
  public:
    v8::Isolate::CreateParams getCreateParams() override { return {}; }
    void customizeIsolate(v8::Isolate* isolate) override {}
    const ActorCacheSharedLruOptions getActorCacheLruOptions() override {
      // TODO(someday): Make this configurable?
      return {
        .softLimit = 16 * (1ull << 20), // 16 MiB
        .hardLimit = 128 * (1ull << 20), // 128 MiB
        .staleTimeout = 30 * kj::SECONDS,
        .dirtyListByteLimit = 8 * (1ull << 20), // 8 MiB
        .maxKeysPerRpc = 128,

        // For now, we use `neverFlush` to implement in-memory-only actors.
        // See WorkerService::getActor().
        .neverFlush = true
      };
    }
    kj::Own<void> enterStartupJs(
        jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    kj::Own<void> enterStartupPython(
        jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    kj::Own<void> enterDynamicImportJs(
        jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    kj::Own<void> enterLoggingJs(
        jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    kj::Own<void> enterInspectorJs(
        jsg::Lock& loc, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    void completedRequest(kj::StringPtr id) const override {}
    bool exitJs(jsg::Lock& lock) const override { return false; }
    void reportMetrics(IsolateObserver& isolateMetrics) const override {}
    kj::Maybe<size_t> checkPbkdfIterations(jsg::Lock& lock, size_t iterations) const override {
      // No limit on the number of iterations in workerd
      return kj::none;
    }
  };

  auto observer = kj::atomicRefcounted<IsolateObserver>();
  auto limitEnforcer = kj::heap<NullIsolateLimitEnforcer>();
  auto api = kj::heap<WorkerdApi>(globalContext->v8System,
                                  featureFlags.asReader(),
                                  *limitEnforcer,
                                  kj::atomicAddRef(*observer),
                                  *memoryCacheProvider);
  auto inspectorPolicy = Worker::Isolate::InspectorPolicy::DISALLOW;
  if (inspectorOverride != kj::none) {
    // For workerd, if the inspector is enabled, it is always fully trusted.
    inspectorPolicy = Worker::Isolate::InspectorPolicy::ALLOW_FULLY_TRUSTED;
  }
  auto isolate = kj::atomicRefcounted<Worker::Isolate>(
      kj::mv(api),
      kj::mv(observer),
      name,
      kj::mv(limitEnforcer),
      inspectorPolicy,
      conf.isServiceWorkerScript() ? Worker::ConsoleMode::INSPECTOR_ONLY : consoleMode);

  // If we are using the inspector, we need to register the Worker::Isolate
  // with the inspector service.
  KJ_IF_SOME(isolateRegistrar, inspectorIsolateRegistrar) {
    isolateRegistrar->registerIsolate(name, isolate.get());
  }

  if (conf.hasModuleFallback()) {
    KJ_REQUIRE(experimental,
               "The module fallback service is an experimental feature. "
               "You must run workerd with `--experimental` to use this feature.");
    // If the config has the moduleFallback option, then we are going to set up the ability
    // to load certain modules from a fallback service. This is generally intended for local
    // dev/testing purposes only.
    auto& apiIsolate = isolate->getApi();
    apiIsolate.setModuleFallbackCallback(
        [address=kj::str(conf.getModuleFallback()), featureFlags=apiIsolate.getFeatureFlags()]
        (jsg::Lock& js,
         kj::StringPtr specifier,
         kj::Maybe<kj::String> referrer,
         jsg::CompilationObserver& observer,
         jsg::ModuleRegistry::ResolveMethod method) mutable
            -> kj::Maybe<kj::OneOf<kj::String, jsg::ModuleRegistry::ModuleInfo>> {
      kj::Maybe<kj::String> jsonPayload;
      bool redirect = false;
      bool prefixed = false;
      kj::Url url;
      kj::StringPtr actualSpecifier = nullptr;
      // TODO(cleanup): This is a bit of a hack based on the current
      // design of the module registry loader algorithms handling of
      // prefixed modules. This will be simplified with the upcoming
      // module registry refactor.
      KJ_IF_SOME(pos, specifier.findLast('/')) {
        auto segment = specifier.slice(pos + 1);
        if (segment.startsWith("node:") ||
            segment.startsWith("cloudflare:") ||
            segment.startsWith("workerd:")) {
          actualSpecifier = segment;
          url.query.add(kj::Url::QueryParam {
            .name = kj::str("specifier"),
            .value = kj::str(segment)
          });
          prefixed = true;
        }
      }
      if (!prefixed) {
        actualSpecifier = specifier;
        if (actualSpecifier.startsWith("/")) {
          actualSpecifier = specifier.slice(1);
        }
        url.query.add(
          kj::Url::QueryParam { kj::str("specifier"), kj::str(specifier) }
        );
      }
      KJ_IF_SOME(ref, referrer) {
        url.query.add(
          kj::Url::QueryParam { kj::str("referrer"), kj::mv(ref) }
        );
      }

      auto spec = url.toString(kj::Url::HTTP_REQUEST);

      {
        // Module loading in workerd is expected to be synchronous but we need to perform
        // an async HTTP request to the fallback service. To accomplish that we wrap the
        // actual request in a kj::Thread, perform the GET, and drop the thread immediately
        // so that the destructor joins the current thread (blocking it). The thread will
        // either set the jsonPayload variable or not.
        kj::Thread loaderThread(
          [&spec,
           referrer=kj::mv(referrer),
           address=address.asPtr(),
           &jsonPayload,
           &redirect,
           method]() mutable {
          try {
            const auto toStr = [](jsg::ModuleRegistry::ResolveMethod method) {
              switch (method) {
                case jsg::ModuleRegistry::ResolveMethod::IMPORT: return "import"_kjc;
                case jsg::ModuleRegistry::ResolveMethod::REQUIRE: return "require"_kjc;
              }
              KJ_UNREACHABLE;
            };

            kj::AsyncIoContext io = kj::setupAsyncIo();

            kj::HttpHeaderTable::Builder builder;
            kj::HttpHeaderId kMethod = builder.add("x-resolve-method");
            auto headerTable = builder.build();

            auto addr = io.provider->getNetwork().parseAddress(address, 80).wait(io.waitScope);

            auto client = kj::newHttpClient(
                io.provider->getTimer(),
                *headerTable,
                *addr, { });

            kj::HttpHeaders headers(*headerTable);
            headers.set(kMethod, toStr(method));
            headers.set(kj::HttpHeaderId::HOST, "localhost"_kj);

            auto request = client->request(kj::HttpMethod::GET, spec, headers, kj::none);

            kj::HttpClient::Response resp = request.response.wait(io.waitScope);

            if (resp.statusCode == 301) {
              // The fallback service responded with a redirect.
              KJ_IF_SOME(loc, resp.headers->get(kj::HttpHeaderId::LOCATION)) {
                redirect = true;
                jsonPayload = kj::str(loc);
              } else {
                KJ_LOG(ERROR, "Fallback service returned a redirect with no location", spec);
              }
            } else if (resp.statusCode != 200) {
              // Failed! Log the body of the respnose, if any, and fall through without
              // setting jsonPayload to signal that the fallback service failed to return
              // a module for this specifier.
              auto payload = resp.body->readAllText().wait(io.waitScope);
              KJ_LOG(ERROR, "Fallback service failed to fetch module", payload, spec);
            } else {
              jsonPayload = resp.body->readAllText().wait(io.waitScope);
            }
          } catch (...) {
            auto exception = kj::getCaughtExceptionAsKj();
            KJ_LOG(ERROR, "Fallback service failed to fetch module", exception, spec);
          }
        });
      }

      KJ_IF_SOME(payload, jsonPayload) {
        // If the payload is empty then the fallback service failed to fetch the module.
        if (payload.size() == 0) return kj::none;

        // If redirect is true then the fallback service returned a 301 redirect. The
        // payload is the specifier of the new target module.
        if (redirect) {
          return kj::Maybe(kj::mv(payload));
        }

        // The response from the fallback service must be a valid JSON serialization
        // of the workerd module configuration. If it is not, or if there is any other
        // error when processing here, we'll log the exception and return nothing.
        try {
          capnp::MallocMessageBuilder moduleMessage;
          capnp::JsonCodec json;
          json.handleByAnnotation<config::Worker::Module>();
          auto moduleBuilder = moduleMessage.initRoot<config::Worker::Module>();
          json.decode(payload, moduleBuilder);

          // If the module fallback service returns a name in the module then it has to
          // match the specifier we passed in. This is an optional sanity check.
          if (moduleBuilder.hasName()) {
            if (moduleBuilder.getName() != actualSpecifier) {
              KJ_LOG(ERROR,
                    "Fallback service failed to fetch module: returned module "
                    "name does not match specifier",
                    moduleBuilder.getName(), actualSpecifier);
              return kj::none;
            }
          } else {
            moduleBuilder.setName(kj::str(actualSpecifier));
          }

          auto module = WorkerdApi::tryCompileModule(js, moduleBuilder, observer, featureFlags);
          if (module == kj::none) {
            KJ_LOG(ERROR, "Fallback service does not support this module type",
                moduleBuilder.which());
          }
          return module;
        } catch (...) {
          auto exception = kj::getCaughtExceptionAsKj();
          KJ_LOG(ERROR, "Fallback service failed to fetch module", exception, spec);
          return kj::none;
        }
      }

      // If we got here, no jsonPayload was received and we return nothing.
      return kj::none;
    });
  }

  auto script = isolate->newScript(
      name, WorkerdApi::extractSource(name, conf, errorReporter, extensions),
      IsolateObserver::StartType::COLD, false, errorReporter);

  kj::Vector<FutureSubrequestChannel> subrequestChannels;
  kj::Vector<FutureActorChannel> actorChannels;

  auto confBindings = conf.getBindings();
  using Global = WorkerdApi::Global;
  kj::Vector<Global> globals(confBindings.size());
  for (auto binding: confBindings) {
    KJ_IF_SOME(global, createBinding(name, conf, binding, errorReporter,
                                     subrequestChannels, actorChannels, actorConfigs,
                                     experimental)) {
      globals.add(kj::mv(global));
    }
  }

  auto worker = kj::atomicRefcounted<Worker>(
      kj::mv(script),
      kj::atomicRefcounted<WorkerObserver>(),
      [&](jsg::Lock& lock, const Worker::Api& api, v8::Local<v8::Object> target) {
        return WorkerdApi::from(api).compileGlobals(
            lock, globals, target, 1);
      },
      IsolateObserver::StartType::COLD,
      nullptr,          // systemTracer -- TODO(beta): factor out
      Worker::Lock::TakeSynchronously(kj::none),
      errorReporter);

  {
    worker->runInLockScope(Worker::Lock::TakeSynchronously(kj::none), [&](Worker::Lock& lock) {
      lock.validateHandlers(errorReporter);
    });
  }

  auto linkCallback =
      [this, name, conf, subrequestChannels = kj::mv(subrequestChannels),
       actorChannels = kj::mv(actorChannels)](WorkerService& workerService) mutable {
    WorkerService::LinkedIoChannels result{.alarmScheduler = *alarmScheduler};

    auto services = kj::heapArrayBuilder<Service*>(subrequestChannels.size() +
              IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT);

    Service& globalService = lookupService(conf.getGlobalOutbound(),
        kj::str("Worker \"", name, "\"'s globalOutbound"));

    // Bind both "next" and "null" to the global outbound. (The difference between these is a
    // legacy artifact that no one should be depending on.)
    static_assert(IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT == 2);
    services.add(&globalService);
    services.add(&globalService);

    for (auto& channel: subrequestChannels) {
      services.add(&lookupService(channel.designator, kj::mv(channel.errorContext)));
    }

    result.subrequest = services.finish();

    result.actor = KJ_MAP(channel, actorChannels) -> kj::Maybe<WorkerService::ActorNamespace&> {
      WorkerService* targetService = &workerService;
      if (channel.designator.hasServiceName()) {
        auto& svc = KJ_UNWRAP_OR(this->services.find(channel.designator.getServiceName()), {
          // error was reported earlier
          return kj::none;
        });
        targetService = dynamic_cast<WorkerService*>(svc.get());
        if (targetService == nullptr) {
          // error was reported earlier
          return kj::none;
        }
      }

      // (If getActorNamespace() returns null, an error was reported earlier.)
      return targetService->getActorNamespace(channel.designator.getClassName());
    };

    if (conf.hasCacheApiOutbound()) {
      result.cache = lookupService(conf.getCacheApiOutbound(),
                                   kj::str("Worker \"", name, "\"'s cacheApiOutbound"));
    }

    auto actorStorageConf = conf.getDurableObjectStorage();
    if (actorStorageConf.isLocalDisk()) {
      kj::StringPtr diskName = actorStorageConf.getLocalDisk();
      KJ_IF_SOME(svc, this->services.find(actorStorageConf.getLocalDisk())) {
        auto diskSvc = dynamic_cast<DiskDirectoryService*>(svc.get());
        if (diskSvc == nullptr) {
          reportConfigError(kj::str("service ", name, ": durableObjectStorage config refers "
              "to the service \"", diskName, "\", but that service is not a local disk service."));
        } else KJ_IF_SOME(dir, diskSvc->getWritable()) {
          result.actorStorage = kj::heap<SqliteDatabase::Vfs>(dir);
        } else {
          reportConfigError(kj::str("service ", name, ": durableObjectStorage config refers "
              "to the disk service \"", diskName, "\", but that service is defined read-only."));
        }
      } else {
        reportConfigError(kj::str("service ", name, ": durableObjectStorage config refers "
            "to a service \"", diskName, "\", but no such service is defined."));
      }
    }

    kj::HashMap<kj::StringPtr, WorkerService::ActorNamespace&> durableNamespacesByUniqueKey;
    for(auto& [className, ns] : workerService.getActorNamespaces()) {
      KJ_IF_SOME(config, ns->getConfig().tryGet<Server::Durable>()) {
        auto& actorNs = ns; // clangd gets confused trying to use ns directly in the capture below??

        alarmScheduler->registerNamespace(config.uniqueKey,
            [&actorNs](kj::String id) -> kj::Own<WorkerInterface> {
          return actorNs->getActor(kj::mv(id), IoChannelFactory::SubrequestMetadata{});
        });
      }
    }

    return result;
  };

  return kj::heap<WorkerService>(globalContext->threadContext, kj::mv(worker),
                                 kj::mv(errorReporter.defaultEntrypoint),
                                 kj::mv(errorReporter.namedEntrypoints), localActorConfigs,
                                 kj::mv(linkCallback), KJ_BIND_METHOD(*this, abortAllActors));
}

// =======================================================================================

kj::Own<Server::Service> Server::makeService(
    config::Service::Reader conf,
    kj::HttpHeaderTable::Builder& headerTableBuilder,
    capnp::List<config::Extension>::Reader extensions) {
  kj::StringPtr name = conf.getName();

  switch (conf.which()) {
    case config::Service::UNSPECIFIED:
      reportConfigError(kj::str(
          "Service named \"", name, "\" does not specify what to serve."));
      return makeInvalidConfigService();

    case config::Service::EXTERNAL:
      return makeExternalService(name, conf.getExternal(), headerTableBuilder);

    case config::Service::NETWORK:
      return makeNetworkService(conf.getNetwork());

    case config::Service::WORKER:
      return makeWorker(name, conf.getWorker(), extensions);

    case config::Service::DISK:
      return makeDiskDirectoryService(name, conf.getDisk(), headerTableBuilder);
  }

  reportConfigError(kj::str(
      "Service named \"", name, "\" has unrecognized type. Was the config compiled with a "
      "newer version of the schema?"));
  return makeInvalidConfigService();
}

void Server::taskFailed(kj::Exception&& exception) {
  fatalFulfiller->reject(kj::mv(exception));
}

Server::Service& Server::lookupService(
    config::ServiceDesignator::Reader designator, kj::String errorContext) {
  kj::StringPtr targetName = designator.getName();
  Service* service = KJ_UNWRAP_OR(services.find(targetName), {
    reportConfigError(kj::str(
        errorContext, " refers to a service \"", targetName,
        "\", but no such service is defined."));
    return *invalidConfigServiceSingleton;
  });

  if (designator.hasEntrypoint()) {
    kj::StringPtr entrypointName = designator.getEntrypoint();
    if (WorkerService* worker = dynamic_cast<WorkerService*>(service)) {
      KJ_IF_SOME(ep, worker->getEntrypoint(entrypointName)) {
        return ep;
      } else {
        reportConfigError(kj::str(
            errorContext, " refers to service \"", targetName, "\" with a named entrypoint \"",
            entrypointName, "\", but \"", targetName, "\" has no such named entrypoint."));
        return *invalidConfigServiceSingleton;
      }
    } else {
      reportConfigError(kj::str(
          errorContext, " refers to service \"", targetName, "\" with a named entrypoint \"",
          entrypointName, "\", but \"", targetName, "\" is not a Worker, so does not have any "
          "named entrypoints."));
      return *invalidConfigServiceSingleton;
    }
  } else {
    return *service;
  }
}

// =======================================================================================

class Server::HttpListener final: public kj::Refcounted {
public:
  HttpListener(Server& owner, kj::Own<kj::ConnectionReceiver> listener, Service& service,
               kj::StringPtr physicalProtocol, kj::Own<HttpRewriter> rewriter,
               kj::HttpHeaderTable& headerTable, kj::Timer& timer,
               capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
      : owner(owner), listener(kj::mv(listener)), service(service),
        headerTable(headerTable), timer(timer),
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

        KJ_IF_SOME(remote,
            kj::dynamicDowncastIfAvailable<kj::NetworkPeerIdentity>(*peerId)) {
          cfBlobJson = kj::str("{\"clientIp\": \"", escapeJsonString(remote.toString()), "\"}");
        } else KJ_IF_SOME(local,
            kj::dynamicDowncastIfAvailable<kj::LocalPeerIdentity>(*peerId)) {
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

      static auto constexpr listen = [](kj::Own<HttpListener> self,
                                        kj::Own<Connection> conn,
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
  Service& service;
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

    // Capnp server not initialized. Create in now.
    auto& s = capnpServer.emplace(kj::heap<WorkerdBootstrapImpl>(*this));
    return s.accept(conn);
  }

  class WorkerdBootstrapImpl final: public rpc::WorkerdBootstrap::Server {
  public:
    WorkerdBootstrapImpl(HttpListener& parent): parent(parent) {}

    kj::Promise<void> startEvent(StartEventContext context) {
      // TODO(someday): Use cfBlobJson from the connection if there is one, or from RPC params
      //   if we add that? (Note that if a connection-level cf blob exists, it should take
      //   priority; we should only accept a cf blob from the client if we have a cfBlobHeader
      //   configrued, which hints that this service trusts the client to provide the cf blob.)

      context.initResults(capnp::MessageSize {4, 1}).setDispatcher(
          kj::heap<EventDispatcherImpl>(parent, parent.service.startRequest({})));
      return kj::READY_NOW;
    }

  private:
    HttpListener& parent;
  };

  class EventDispatcherImpl final: public rpc::EventDispatcher::Server {
  public:
    EventDispatcherImpl(HttpListener& parent, kj::Own<WorkerInterface> worker)
        : parent(parent), worker(kj::mv(worker)) {}

    kj::Promise<void> getHttpService(GetHttpServiceContext context) override {
      context.initResults(capnp::MessageSize{4, 1})
          .setHttp(parent.httpOverCapnpFactory.kjToCapnp(getWorker()));
      return kj::READY_NOW;
    }

    kj::Promise<void> sendTraces(SendTracesContext context) override {
      throwUnsupported();
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
      auto customEvent = kj::heap<api::JsRpcSessionCustomEventImpl>(
          api::JsRpcSessionCustomEventImpl::WORKER_RPC_EVENT_TYPE);

      auto cap = customEvent->getCap();
      capnp::PipelineBuilder<JsRpcSessionResults> pipelineBuilder;
      pipelineBuilder.setTopLevel(cap);
      context.setPipeline(pipelineBuilder.build());
      context.getResults().setTopLevel(kj::mv(cap));

      auto worker = getWorker();
      return worker->customEvent(kj::mv(customEvent)).ignoreResult().attach(kj::mv(worker));
    }

  private:
    HttpListener& parent;
    kj::Maybe<kj::Own<WorkerInterface>> worker;

    kj::Own<WorkerInterface> getWorker() {
      auto result = kj::mv(KJ_ASSERT_NONNULL(worker,
          "EventDispatcher can only be used for one request"));
      worker = kj::none;
      return result;
    }

    [[noreturn]] void throwUnsupported() {
      JSG_FAIL_REQUIRE(Error, "RPC connections don't yet support this event type.");
    }
  };

  struct Connection final: public kj::HttpService, public kj::HttpServerErrorHandler {
    Connection(HttpListener& parent, kj::Maybe<kj::String> cfBlobJson)
        : parent(parent), cfBlobJson(kj::mv(cfBlobJson)),
          listedHttp(parent.owner, parent.timer, parent.headerTable, *this, kj::HttpServerSettings {
            .errorHandler = *this,
            .webSocketCompressionMode = kj::HttpServerSettings::MANUAL_COMPRESSION
          }) {}

    HttpListener& parent;
    kj::Maybe<kj::String> cfBlobJson;
    ListedHttpServer listedHttp;

    class ResponseWrapper final: public kj::HttpService::Response {
    public:
      ResponseWrapper(kj::HttpService::Response& inner, HttpRewriter& rewriter)
          : inner(inner), rewriter(rewriter) {}

      kj::Own<kj::AsyncOutputStream> send(
          uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
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

    kj::Promise<void> request(
        kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
        kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override {
      TRACE_EVENT("workerd", "Connection:request()");
      IoChannelFactory::SubrequestMetadata metadata;
      metadata.cfBlobJson = cfBlobJson.map([](kj::StringPtr s) { return kj::str(s); });

      Response* wrappedResponse = &response;
      kj::Own<ResponseWrapper> ownResponse;
      if (parent.rewriter->needsRewriteResponse()) {
        wrappedResponse = ownResponse = kj::heap<ResponseWrapper>(response, *parent.rewriter);
      }

      if (parent.rewriter->needsRewriteRequest() || cfBlobJson != kj::none) {
        auto rewrite = KJ_UNWRAP_OR(
            parent.rewriter->rewriteIncomingRequest(
                url, parent.physicalProtocol, headers, metadata.cfBlobJson), {
          co_return co_await response.sendError(400, "Bad Request", parent.headerTable);
        });
        auto worker = parent.service.startRequest(kj::mv(metadata));
        co_return co_await worker->request(method, url, *rewrite.headers, requestBody,
                                           *wrappedResponse);
      } else {
        auto worker = parent.service.startRequest(kj::mv(metadata));
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
      KJ_LOG(ERROR, kj::str("Uncaught exception: ", exception));
      KJ_IF_SOME(r, response) {
        co_return co_await r.sendError(500, "Internal Server Error", parent.headerTable);
      }
    }
  };
};

kj::Promise<void> Server::listenHttp(
    kj::Own<kj::ConnectionReceiver> listener, Service& service,
    kj::StringPtr physicalProtocol, kj::Own<HttpRewriter> rewriter) {
  auto obj = kj::refcounted<HttpListener>(*this, kj::mv(listener), service,
                                          physicalProtocol, kj::mv(rewriter),
                                          globalContext->headerTable, timer,
                                          globalContext->httpOverCapnpFactory);
  co_return co_await obj->run();
}

// =======================================================================================
// Server::run()

kj::Promise<void> Server::handleDrain(kj::Promise<void> drainWhen) {
  co_await drainWhen;
  TRACE_EVENT("workerd", "Server::handleDrain()");
  // Tell all HttpServers to drain. This causes them to disconnect any connections that don't
  // have a request in-flight.
  auto drainPromises = kj::heapArrayBuilder<kj::Promise<void>>(httpServers.size());
  for (auto& httpServer: httpServers) {
    drainPromises.add(httpServer.httpServer.drain());
  }
  co_await kj::joinPromisesFailFast(drainPromises.finish());
}

kj::Promise<void> Server::run(jsg::V8System& v8System, config::Config::Reader config,
                              kj::Promise<void> drainWhen) {
  TRACE_EVENT("workerd", "Server.run");
  kj::HttpHeaderTable::Builder headerTableBuilder;
  globalContext = kj::heap<GlobalContext>(*this, v8System, headerTableBuilder);
  invalidConfigServiceSingleton = kj::heap<InvalidConfigService>();

  auto [ fatalPromise, fatalFulfiller ] = kj::newPromiseAndFulfiller<void>();
  this->fatalFulfiller = kj::mv(fatalFulfiller);

  auto forkedDrainWhen = handleDrain(kj::mv(drainWhen)).fork();

  startServices(v8System, config, headerTableBuilder, forkedDrainWhen);

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

  alarmScheduler = kj::heap<AlarmScheduler>(clock, timer, *vfs, kj::Path({"alarms.sqlite"}))
      .attach(kj::mv(vfs));
}

// Configure and start the inspector socket, returning the port the socket started on.
uint startInspector(kj::StringPtr inspectorAddress,
                    Server::InspectorServiceIsolateRegistrar& registrar) {
  static constexpr uint UNASSIGNED_PORT = 0;
  static constexpr uint DEFAULT_PORT = 9229;
  kj::MutexGuarded<uint> inspectorPort(UNASSIGNED_PORT);

  kj::Thread thread([inspectorAddress, &inspectorPort, &registrar](){
    kj::AsyncIoContext io = kj::setupAsyncIo();

    kj::HttpHeaderTable::Builder headerTableBuilder;

    // Create the special inspector service.
    auto inspectorService(
        kj::heap<Server::InspectorService>(io.provider->getTimer(), headerTableBuilder, registrar));
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
    auto listen = (kj::coCapture([&network, &inspectorAddress, &inspectorPort,
                                   &inspectorService]() -> kj::Promise<void> {
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
  return inspectorPort.when(
    [](const uint& port) { return port != UNASSIGNED_PORT; },
    [](const uint& port) { return port; }
  );
}

void Server::startServices(jsg::V8System& v8System, config::Config::Reader config,
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
                Durable {
                    .uniqueKey = kj::str(ns.getUniqueKey()),
                    .isEvictable = !ns.getPreventEviction() });
            continue;
          case config::Worker::DurableObjectNamespace::EPHEMERAL_LOCAL:
            if (!experimental) {
              reportConfigError(kj::str(
                  "Ephemeral objects (Durable Object namespaces with type 'ephemeralLocal') are an "
                  "experimental feature which may change or go away in the future. You must run "
                  "workerd with `--experimental` to use this feature."));
            }
            serviceActorConfigs.insert(kj::str(ns.getClassName()),
                Ephemeral { .isEvictable = !ns.getPreventEviction() });
            continue;
        }
        reportConfigError(kj::str(
            "Encountered unknown DurableObjectNamespace type in service \"", name,
            "\", class \"", ns.getClassName(), "\". Was the config compiled with a newer version "
            "of the schema?"));
      }

      switch (workerConf.getDurableObjectStorage().which()) {
        case config::Worker::DurableObjectStorage::NONE:
          if (hadDurable) {
            reportConfigError(kj::str(
                "Worker service \"", name, "\" implements durable object classes but has "
                "`durableObjectStorage` set to `none`."));
          }
          goto validDurableObjectStorage;
        case config::Worker::DurableObjectStorage::IN_MEMORY:
        case config::Worker::DurableObjectStorage::LOCAL_DISK:
          goto validDurableObjectStorage;
      }
      reportConfigError(kj::str(
          "Encountered unknown durableObjectStorage type in service \"", name,
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
        stream->write(message.begin(), message.size());
      } catch (kj::Exception& e) {
        KJ_LOG(ERROR, e);
      }
    }
    inspectorIsolateRegistrar = kj::mv(registrar);
  }

  // Second pass: Build services.
  for (auto serviceConf: config.getServices()) {
    kj::StringPtr name = serviceConf.getName();
    auto service = makeService(serviceConf, headerTableBuilder, config.getExtensions());

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

    auto service = kj::heap<NetworkService>(
        globalContext->headerTable, timer, entropySource,
        kj::mv(publicNetwork), kj::mv(tlsNetwork), *tls).attach(kj::mv(tls));

    return decltype(services)::Entry {
      kj::str("internet"_kj),
      kj::mv(service)
    };
  });

  // Start the alarm scheduler before linking services
  startAlarmScheduler(config);

  // Third pass: Cross-link services.
  for (auto& service: services) {
    service.value->link();
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

    Service& service = lookupService(sock.getService(), kj::str("Socket \"", name, "\""));

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
      reportConfigError(kj::str(
          "Socket \"", name, "\" has no address in the config, so must be specified on the "
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
    reportConfigError(kj::str(
        "Encountered unknown socket type in \"", name, "\". Was the config compiled with a "
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
                     kj::Own<kj::TlsContext> tls)
          -> PromisedReceived {
        auto port = co_await promise;
        co_return tls->wrapPort(kj::mv(port)).attach(kj::mv(tls));
      })(kj::mv(listener), kj::mv(t));
    }

    // Need to create rewriter before waiting on anything since `headerTableBuilder` will no longer
    // be available later.
    auto rewriter = kj::heap<HttpRewriter>(httpOptions, headerTableBuilder);

    auto handle = kj::coCapture(
        [this, &service, rewriter = kj::mv(rewriter), physicalProtocol, name]
        (kj::Promise<kj::Own<kj::ConnectionReceiver>> promise)
            mutable -> kj::Promise<void> {
      TRACE_EVENT("workerd", "setup listenHttp");
      auto listener = co_await promise;
      KJ_IF_SOME(stream, controlOverride) {
        auto message = kj::str("{\"event\":\"listen\",\"socket\":\"", name, "\",\"port\":", listener->getPort(), "}\n");
        try {
          stream->write(message.begin(), message.size());
        } catch (kj::Exception& e) {
          KJ_LOG(ERROR, e);
        }
      }
      co_await listenHttp(kj::mv(listener), service, physicalProtocol, kj::mv(rewriter));
    });
    tasks.add(handle(kj::mv(listener)).exclusiveJoin(forkedDrainWhen.addBranch()));
  }

  for (auto& unmatched: socketOverrides) {
    reportConfigError(kj::str(
        "Config did not define any socket named \"", unmatched.key, "\" to match the override "
        "provided on the command line."));
  }

  for (auto& unmatched: externalOverrides) {
    reportConfigError(kj::str(
        "Config did not define any external service named \"", unmatched.key, "\" to match the "
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

    reportConfigError(kj::str(
        "Config did not define any disk service named \"", unmatched.key, "\" to match the "
        "override provided on the command line."));
  }

  co_await tasks.onEmpty();

  // Give a chance for any errors to bubble up before we return success. In particular
  // Server::taskFailed() fulfills `fatalFulfiller`, which causes the server to exit with an error.
  // But the `TaskSet` may have become empty at the same time. We want the error to win the race
  // against the success.
  //
  // TODO(cleanup): A better solution wolud be for `TaskSet` to have a new variant of the
  //   `onEmpty()` method like `onEmptyOrException()`, which propagates any exception thrown by
  //   any task.
  co_await kj::evalLast([]() {});
}

// =======================================================================================
// Server::test()

kj::Promise<bool> Server::test(jsg::V8System& v8System, config::Config::Reader config,
                               kj::StringPtr servicePattern,
                               kj::StringPtr entrypointPattern) {
  kj::HttpHeaderTable::Builder headerTableBuilder;
  globalContext = kj::heap<GlobalContext>(*this, v8System, headerTableBuilder);
  invalidConfigServiceSingleton = kj::heap<InvalidConfigService>();

  auto [ fatalPromise, fatalFulfiller ] = kj::newPromiseAndFulfiller<void>();
  this->fatalFulfiller = kj::mv(fatalFulfiller);

  auto forkedDrainWhen = kj::Promise<void>(kj::NEVER_DONE).fork();

  startServices(v8System, config, headerTableBuilder, forkedDrainWhen);

  // Tests usually do not configure sockets, but they can, especially loopback sockets. Arrange
  // to wait on them. Crash if listening fails.
  auto listenPromise = listenOnSockets(config, headerTableBuilder, forkedDrainWhen,
                                       /* forTest = */ true)
      .eagerlyEvaluate([](kj::Exception&& e) noexcept {
    kj::throwFatalException(kj::mv(e));
  });

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
    bool result = co_await req->test();
    if (result) {
      ++passCount;
    } else {
      ++failCount;
    }
    KJ_LOG(DBG, kj::str(result ? "[ PASS ] "_kj : "[ FAIL ] "_kj, name));
  };

  for (auto& service: services) {
    if (serviceGlob.matches(service.key)) {
      if (service.value->hasHandler("test"_kj) && entrypointGlob.matches("default"_kj)) {
        co_await doTest(*service.value, service.key);
      }

      if (WorkerService* worker = dynamic_cast<WorkerService*>(service.value.get())) {
        for (auto& name: worker->getEntrypointNames()) {
          if (entrypointGlob.matches(name)) {
            Service& ep = KJ_ASSERT_NONNULL(worker->getEntrypoint(name));
            if (ep.hasHandler("test"_kj)) {
              co_await doTest(ep, kj::str(service.key, ':', name));
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
