// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "server.h"
#include <kj/debug.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/compat/url.h>
#include <kj/encoding.h>
#include <kj/map.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker-entrypoint.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/setup.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <time.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <workerd/io/actor-cache.h>
#include <workerd/api/actor-state.h>
#include "workerd-api.h"

namespace workerd::server {

namespace {

struct PemData {
  kj::String type;
  kj::Array<byte> data;
};

static kj::Maybe<PemData> decodePem(kj::ArrayPtr<const char> text) {
  // Decode PEM format using OpenSSL helpers.
  //
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
    return nullptr;
  }
  kj::Array<char> nameArr(namePtr, strlen(namePtr) + 1, disposer);
  KJ_DEFER(OPENSSL_free(headerPtr));
  kj::Array<kj::byte> data(dataPtr, dataLen, disposer);

  return PemData { kj::String(kj::mv(nameArr)), kj::mv(data) };
}

static kj::String httpTime(kj::Date date) {
  // Returns a time string in the format HTTP likes to use.

  time_t time = (date - kj::UNIX_EPOCH) / kj::SECONDS;
  struct tm tm;
  KJ_ASSERT(gmtime_r(&time, &tm) == &tm);
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

class EmptyReadOnlyActorStorageImpl final: public rpc::ActorStorage::Stage::Server {
  // An ActorStorage implementation which will always respond to reads as if the state is empty,
  // and will fail any writes.
public:
  kj::Promise<void> get(GetContext context) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> getMultiple(GetMultipleContext context) override {
    return context.getParams().getStream().endRequest(capnp::MessageSize {2, 0})
        .send().ignoreResult();
  }
  kj::Promise<void> list(ListContext context) override {
    return context.getParams().getStream().endRequest(capnp::MessageSize {2, 0})
        .send().ignoreResult();
  }
  kj::Promise<void> getAlarm(GetAlarmContext context) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> txn(TxnContext context) override {
    auto results = context.getResults(capnp::MessageSize {2, 1});
    results.setTransaction(kj::heap<TransactionImpl>());
    return kj::READY_NOW;
  }

private:
  class TransactionImpl final: public rpc::ActorStorage::Stage::Transaction::Server {
  protected:
    kj::Promise<void> get(GetContext context) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> getMultiple(GetMultipleContext context) override {
      return context.getParams().getStream().endRequest(capnp::MessageSize {2, 0})
          .send().ignoreResult();
    }
    kj::Promise<void> list(ListContext context) override {
      return context.getParams().getStream().endRequest(capnp::MessageSize {2, 0})
          .send().ignoreResult();
    }
    kj::Promise<void> getAlarm(GetAlarmContext context) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> commit(CommitContext context) override {
      return kj::READY_NOW;
    }
  };
};

}  // namespace

// =======================================================================================

Server::Server(kj::Filesystem& fs, kj::Timer& timer, kj::Network& network,
               kj::EntropySource& entropySource, kj::Function<void(kj::String)> reportConfigError)
    : fs(fs), timer(timer), network(network), entropySource(entropySource),
      reportConfigError(kj::mv(reportConfigError)), tasks(*this) {}

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
        httpOverCapnpFactory(byteStreamFactory, headerTableBuilder),
        threadContext(server.timer, server.entropySource,
            headerTableBuilder, httpOverCapnpFactory,
            byteStreamFactory,
            false /* isFiddle -- TODO(beta): support */),
        headerTable(headerTableBuilder.getFutureTable()) {}
};

class Server::Service {
public:
  virtual void link() {}
  // Cross-links this service with other services. Must be called once before `startRequest()`.

  virtual kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata) = 0;
  // Begin an incoming request. Returns a `WorkerInterface` object that will be used for one
  // request then discarded.
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

  KJ_IF_MAYBE(h, certificateHost) {
    return network.parseAddress(addrStr, defaultPort)
        .then([certificateHost = *h, context = kj::mv(context)]
              (kj::Own<kj::NetworkAddress> addr) mutable {
      return context->wrapAddress(kj::mv(addr), certificateHost).attach(kj::mv(context));
    });
  } else {
    // Wrap the `Network` itself so we can use the TLS implementation's `parseAddress()` to extract
    // the authority from the address.
    auto tlsNetwork = context->wrapNetwork(network);
    return tlsNetwork->parseAddress(addrStr, defaultPort).attach(kj::mv(tlsNetwork))
        .then([context = kj::mv(context)](kj::Own<kj::NetworkAddress> addr) mutable {
      return addr.attach(kj::mv(context));
    });
  }
}

// =======================================================================================

class Server::HttpRewriter {
  // Helper to apply config::HttpOptions.
  //
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
  }

  bool hasCfBlobHeader() {
    return cfBlobHeader != nullptr;
  }

  bool needsRewriteRequest() {
    return style == config::HttpOptions::Style::HOST
        || cfBlobHeader != nullptr
        || !requestInjector.empty();
  }

  struct Rewritten {
    // Attach this to the promise returned by request().

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
      KJ_IF_MAYBE(h, forwardedProtoHeader) {
        result.headers->set(*h, kj::mv(parsed.scheme));
      }
      url = result.ownUrl = parsed.toString(kj::Url::HTTP_REQUEST);
    }

    KJ_IF_MAYBE(h, cfBlobHeader) {
      KJ_IF_MAYBE(b, cfBlobJson) {
        result.headers->set(*h, *b);
      } else {
        result.headers->unset(*h);
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
      parsed.host = kj::str(KJ_UNWRAP_OR_RETURN(headers.get(kj::HttpHeaderId::HOST), nullptr));

      KJ_IF_MAYBE(h, forwardedProtoHeader) {
        KJ_IF_MAYBE(s, headers.get(*h)) {
          parsed.scheme = kj::str(*s);
          result.headers->unset(*h);
        }
      }

      if (parsed.scheme == nullptr) parsed.scheme = kj::str(physicalProtocol);

      url = result.ownUrl = parsed.toString(kj::Url::HTTP_PROXY_REQUEST);
    }

    KJ_IF_MAYBE(h, cfBlobHeader) {
      KJ_IF_MAYBE(b, headers.get(*h)) {
        cfBlobJson = kj::str(*b);
        result.headers->unset(*h);
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

private:
  config::HttpOptions::Style style;
  kj::Maybe<kj::HttpHeaderId> forwardedProtoHeader;
  kj::Maybe<kj::HttpHeaderId> cfBlobHeader;

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
        KJ_IF_MAYBE(v, header.value) {
          headers.set(header.id, *v);
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

class Server::InvalidConfigService final: public Service {
  // Service used when the service's config is invalid.

public:
  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    JSG_FAIL_REQUIRE(Error, "Service cannot handle requests because its config is invalid.");
  }
};

kj::Own<Server::Service> Server::makeInvalidConfigService() {
  // Return a fake Own pointing to the singleton.
  return { invalidConfigServiceSingleton.get(), kj::NullDisposer::instance };
}

class PromisedNetworkAddress final: public kj::NetworkAddress {
  // A NetworkAddress whose connect() method waits for a Promise<NetworkAddress> and then forwards
  // to it. Used by ExternalHttpService so that we don't have to wait for DNS lookup before the
  // server can start.
  //
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
    KJ_IF_MAYBE(a, addr) {
      return a->get()->connect();
    } else {
      return promise.addBranch().then([this]() {
        return KJ_ASSERT_NONNULL(addr)->connect();
      });
    }
  }

  kj::Promise<kj::AuthenticatedStream> connectAuthenticated() override {
    KJ_IF_MAYBE(a, addr) {
      return a->get()->connectAuthenticated();
    } else {
      return promise.addBranch().then([this]() {
        return KJ_ASSERT_NONNULL(addr)->connectAuthenticated();
      });
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

class Server::ExternalHttpService final: public Service {
  // Service used when the service is configured as external HTTP service.

public:
  ExternalHttpService(kj::Own<kj::NetworkAddress> addrParam,
                      kj::Own<HttpRewriter> rewriter, kj::HttpHeaderTable& headerTable,
                      kj::Timer& timer, kj::EntropySource& entropySource)
      : addr(kj::mv(addrParam)),
        inner(kj::newHttpClient(timer, headerTable, *addr, {
          .entropySource = entropySource,
          .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION
        })),
        serviceAdapter(kj::newHttpService(*inner)),
        rewriter(kj::mv(rewriter)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return kj::heap<WorkerInterfaceImpl>(*this, kj::mv(metadata));
  }

private:
  kj::Own<kj::NetworkAddress> addr;

  kj::Own<kj::HttpClient> inner;
  kj::Own<kj::HttpService> serviceAdapter;

  kj::Own<HttpRewriter> rewriter;

  class WorkerInterfaceImpl final: public WorkerInterface, private kj::HttpService::Response {
  public:
    WorkerInterfaceImpl(ExternalHttpService& parent, IoChannelFactory::SubrequestMetadata metadata)
        : parent(parent), metadata(kj::mv(metadata)) {}

    kj::Promise<void> request(
        kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
        kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override {
      KJ_REQUIRE(wrappedResponse == nullptr, "object should only receive one request");
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
        ConnectResponse& tunnel) override {
      return parent.serviceAdapter->connect(host, headers, connection, tunnel);
    }

    void prewarm(kj::StringPtr url) override {}
    kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
      throwUnsupported();
    }
    kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override {
      throwUnsupported();
    }
    kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
      throwUnsupported();
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
  kj::StringPtr addrStr = nullptr;
  kj::String ownAddrStr = nullptr;

  KJ_IF_MAYBE(override, externalOverrides.findEntry(name)) {
    addrStr = ownAddrStr = kj::mv(override->value);
    externalOverrides.erase(*override);
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
          kj::mv(addr), kj::mv(rewriter), globalContext->headerTable, timer, entropySource);
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
          kj::mv(addr), kj::mv(rewriter), globalContext->headerTable, timer, entropySource);
    }
  }
  reportConfigError(kj::str(
      "External service named \"", name, "\" has unrecognized protocol. Was the config "
      "compiled with a newer version of the schema?"));
  return makeInvalidConfigService();
}

class Server::NetworkService final: public Service, private WorkerInterface {
  // Service used when the service is configured as network service.

public:
  NetworkService(kj::HttpHeaderTable& headerTable,
                 kj::Timer& timer, kj::EntropySource& entropySource,
                 kj::Own<kj::Network> networkParam,
                 kj::Maybe<kj::Own<kj::Network>> tlsNetworkParam)
      : network(kj::mv(networkParam)), tlsNetwork(kj::mv(tlsNetworkParam)),
        inner(kj::newHttpClient(timer, headerTable, *network, tlsNetwork, {
          .entropySource = entropySource,
          .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION
        })),
        serviceAdapter(kj::newHttpService(*inner)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return { this, kj::NullDisposer::instance };
  }

  kj::Promise<void> connect(
      kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
      ConnectResponse& tunnel) override {
    // This code is hit when the global `connect` function is called in a JS worker script.
    // It represents a proxy-less TCP connection, which means we can simply defer the handling of
    // the connection to the service adapter (likely NetworkHttpClient). Its behaviour will be to
    // connect directly to the host over TCP.
    return serviceAdapter->connect(host, headers, connection, tunnel);
  }

private:
  kj::Own<kj::Network> network;
  kj::Maybe<kj::Own<kj::Network>> tlsNetwork;
  kj::Own<kj::HttpClient> inner;
  kj::Own<kj::HttpService> serviceAdapter;

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override {
    return serviceAdapter->request(method, url, headers, requestBody, response);
  }

  void prewarm(kj::StringPtr url) override {}
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    throwUnsupported();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override {
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
  auto restrictedNetwork = network.restrictPeers(
      KJ_MAP(a, conf.getAllow()) -> kj::StringPtr { return a; },
      KJ_MAP(a, conf.getDeny() ) -> kj::StringPtr { return a; });

  kj::Maybe<kj::Own<kj::Network>> tlsNetwork;
  if (conf.hasTlsOptions()) {
    auto tlsContext = makeTlsContext(conf.getTlsOptions());
    tlsNetwork = tlsContext->wrapNetwork(*restrictedNetwork).attach(kj::mv(tlsContext));
  }

  return kj::heap<NetworkService>(globalContext->headerTable, timer, entropySource,
                                  kj::mv(restrictedNetwork), kj::mv(tlsNetwork));
}

class Server::DiskDirectoryService final: public Service, private WorkerInterface {
  // Service used when the service is configured as disk directory service.

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

private:
  kj::Maybe<const kj::Directory&> writable;
  kj::Own<const kj::ReadableDirectory> readable;
  kj::HttpHeaderTable& headerTable;
  kj::HttpHeaderId hLastModified;
  bool allowDotfiles;

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr urlStr, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override {
    auto url = kj::Url::parse(urlStr);

    bool blockedPath = false;
    kj::Path path = nullptr;
    KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
      path = kj::Path(url.path.releaseAsArray());
    })) {
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
        return response.sendError(404, "Not Found", headerTable);
      }

      auto file = KJ_UNWRAP_OR(readable->tryOpenFile(path), {
        return response.sendError(404, "Not Found", headerTable);
      });

      auto meta = file->stat();

      switch (meta.type) {
        case kj::FsNode::Type::FILE: {
          kj::HttpHeaders headers(headerTable);
          headers.set(kj::HttpHeaderId::CONTENT_TYPE, "application/octet-stream");
          headers.set(hLastModified, httpTime(meta.lastModified));

          // We explicitly set the Content-Length header because if we don't, and we were called
          // by a local Worker (without an actual HTTP connection in between), then the Worker
          // will not see a Content-Length header, but being able to query the content length
          // (especially with HEAD requests) is quite useful.
          // TODO(cleanup): Arguably the implementation of `fetch()` should be adjusted so that
          //   if no `Content-Length` header is returned, but the body size is known via the KJ
          //   HTTP API, then the header shoud be filled in automatically. Unclear if this is safe
          //   to change without a compat flag.
          headers.set(kj::HttpHeaderId::CONTENT_LENGTH, kj::str(meta.size));

          auto out = response.send(200, "OK", headers, meta.size);

          if (method == kj::HttpMethod::HEAD) {
            return kj::READY_NOW;
          } else {
            auto in = kj::heap<kj::FileInputStream>(*file);

            return in->pumpTo(*out, meta.size)
                .ignoreResult()
                .attach(kj::mv(in), kj::mv(out), kj::mv(file));
          }
        }
        case kj::FsNode::Type::DIRECTORY: {
          // Whoooops, we opened a directory. Back up and start over.

          auto dir = readable->openSubdir(path);

          kj::HttpHeaders headers(headerTable);
          headers.set(kj::HttpHeaderId::CONTENT_TYPE, "application/json");
          headers.set(hLastModified, httpTime(meta.lastModified));

          // We intentionally don't provide the expected size here in order to reserve the right
          // to switch to streaming directory listing in the future.
          auto out = response.send(200, "OK", headers);

          if (method == kj::HttpMethod::HEAD) {
            return kj::READY_NOW;
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

            return out->write(content.begin(), content.size())
                .attach(kj::mv(content), kj::mv(out));
          }
        }
        default:
          return response.sendError(406, "Not Acceptable", headerTable);
      }
    } else if (method == kj::HttpMethod::PUT) {
      auto& w = KJ_UNWRAP_OR(writable, {
        return response.sendError(405, "Method Not Allowed", headerTable);
      });

      if (blockedPath) {
        return response.sendError(403, "Unauthorized", headerTable);
      }

      auto replacer = w.replaceFile(path,
          kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
      auto stream = kj::heap<kj::FileOutputStream>(replacer->get());

      return requestBody.pumpTo(*stream).attach(kj::mv(stream))
          .then([this, replacer = kj::mv(replacer), &response](uint64_t) mutable {
        replacer->commit();
        kj::HttpHeaders headers(headerTable);
        response.send(204, "No Content", headers);
      });
    } else {
      return response.sendError(501, "Not Implemented", headerTable);
    }
  }

  kj::Promise<void> connect(kj::StringPtr host, const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection, kj::HttpService::ConnectResponse& response) override {
    throwUnsupported();
  }
  void prewarm(kj::StringPtr url) override {}
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    throwUnsupported();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override {
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
  kj::StringPtr pathStr = nullptr;
  kj::String ownPathStr;

  KJ_IF_MAYBE(override, directoryOverrides.findEntry(name)) {
    pathStr = ownPathStr = kj::mv(override->value);
    directoryOverrides.erase(*override);
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
class Server::InspectorService final: public kj::HttpService, public kj::HttpServerErrorHandler {
  // Implements the interface for the devtools inspector protocol.
  //
  // The InspectorService is created when workerd serve is called using the -i option
  // to define the inspector socket.
public:
  InspectorService(
      kj::Timer& timer,
      kj::HttpHeaderTable::Builder& headerTableBuilder)
      : timer(timer),
        headerTable(headerTableBuilder.getFutureTable()),
        server(timer, headerTable, *this, kj::HttpServerSettings {
          .errorHandler = *this
        }) {}

  kj::Promise<void> handleApplicationError(
      kj::Exception exception, kj::Maybe<kj::HttpService::Response&> response) override {
    KJ_LOG(ERROR, kj::str("Uncaught exception: ", exception));
    KJ_IF_MAYBE(r, response) {
      return r->sendError(500, "Internal Server Error", headerTable);
    } else {
      return kj::READY_NOW;
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
      KJ_IF_MAYBE(pos, url.findLast('/')) {
        auto id = url.slice(*pos + 1);

        KJ_IF_MAYBE(isolate, isolates.find(id)) {
          // If getting the strong ref doesn't work it means that the Worker::Isolate
          // has already been cleaned up. We use a weak ref here in order to keep from
          // having the Worker::Isolate itself having to know anything at all about the
          // IsolateService and the registration process. So instead of having Isolate
          // explicitly clean up after itself we lazily evaluate the weak ref and clean
          // up when necessary.
          KJ_IF_MAYBE(ref, (*isolate)->tryAddStrongRef()) {
            // When using --verbose, we'll output some logging to indicate when the
            // inspector client is attached/detached.
            KJ_LOG(INFO, kj::str("Inspector client attaching [", id, "]"));
            auto webSocket = response.acceptWebSocket(responseHeaders);
            kj::Duration timerOffset = 0 * kj::MILLISECONDS;
            return (*ref)->attachInspector(timer, timerOffset, *webSocket)
                .attach(kj::mv(webSocket), kj::mv(*ref)).catch_([id=kj::str(id)](kj::Exception&& ex)
                    -> kj::Promise<void> {
              if (ex.getType() == kj::Exception::Type::DISCONNECTED) {
                // This likely just means that the inspector client was closed.
                // Nothing to do here but move along.
                KJ_LOG(INFO, kj::str("Inspector client detached [", id, "]"));
                return kj::READY_NOW;
              } else {
                // If it's any other kind of error, propagate it!
                throw ex;
              }
            });
          } else {
            // If we can't get a strong ref to the isolate here, it's been cleaned
            // up. The only thing we're going to do is clean up here and act like
            // nothing happened.
            isolates.erase(id);
          }
        }

        return response.sendError(404, "Unknown worker session", responseHeaders);
      }

      // No / in url!? That's weird
      return response.sendError(400, "Invalid request", responseHeaders);
    }

    // If the request is not a WebSocket request, it must be a GET to fetch details
    // about the implementation.
    if (method != kj::HttpMethod::GET) {
      return response.sendError(501, "Unsupported Operation", responseHeaders);
    }

    if (url.endsWith("/json/version")) {
      responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, "application/json"_kj);
      auto content = kj::str("{\"Browser\": \"workerd\", \"Protocol-Version\": \"1.3\" }");
      auto out = response.send(200, "OK", responseHeaders, content.size());
      return out->write(content.begin(), content.size()).attach(kj::mv(content), kj::mv(out));
    } else if (url.endsWith("/json") || url.endsWith("/json/list")) {
      responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, "application/json"_kj);

      auto baseWsUrl = KJ_UNWRAP_OR(headers.get(kj::HttpHeaderId::HOST), {
        return response.sendError(400, "Bad Request", responseHeaders);
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
        // want to refactor this such thatthe WorkerService holds a handle to the registration
        // as opposed to using this lazy cleanup mechanism. For now, however, this is
        // sufficient.
        KJ_IF_MAYBE(ref, entry.value->tryAddStrongRef()) {
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
      return out->write(content.begin(), content.size()).attach(kj::mv(content), kj::mv(out));
    }

    return response.sendError(500, "Not yet implemented", responseHeaders);
  }

  kj::Promise<void> listen(kj::Own<kj::ConnectionReceiver> listener) {
    return server.listenHttp(*listener).attach(kj::mv(listener));
  }

  void registerIsolate(kj::StringPtr name, Worker::Isolate* isolate) {
    isolates.insert(kj::str(name), isolate->getWeakRef());
  }

private:
  kj::Timer& timer;
  kj::HttpHeaderTable& headerTable;
  kj::HashMap<kj::String, kj::Own<const Worker::Isolate::WeakIsolateRef>> isolates;
  kj::HttpServer server;

  friend class Registration;
};

kj::Own<Server::InspectorService> Server::makeInspectorService(
    kj::HttpHeaderTable::Builder& headerTableBuilder) {
  return kj::heap<InspectorService>(timer, headerTableBuilder);
}

// =======================================================================================

class Server::WorkerService final: public Service, private kj::TaskSet::ErrorHandler,
                                   private IoChannelFactory, private TimerChannel,
                                   private LimitEnforcer {
public:
  class ActorNamespace;

  struct LinkedIoChannels {
    // I/O channels, delivered when link() is called.
    kj::Array<Service*> subrequest;
    kj::Array<kj::Maybe<ActorNamespace&>> actor;  // null = configuration error
    kj::Maybe<Service&> cache;
  };
  using LinkCallback = kj::Function<LinkedIoChannels(WorkerService&)>;

  WorkerService(ThreadContext& threadContext, kj::Own<const Worker> worker,
                kj::HashSet<kj::String> namedEntrypointsParam,
                const kj::HashMap<kj::String, ActorConfig>& actorClasses,
                LinkCallback linkCallback)
      : threadContext(threadContext), worker(kj::mv(worker)),
        ioChannels(kj::mv(linkCallback)),
        waitUntilTasks(*this) {
    namedEntrypoints.reserve(namedEntrypointsParam.size());
    for (auto& ep: namedEntrypointsParam) {
      kj::StringPtr epPtr = ep;
      namedEntrypoints.insert(kj::mv(ep), EntrypointService(*this, epPtr));
    }

    actorNamespaces.reserve(actorClasses.size());
    for (auto& entry: actorClasses) {
      ActorNamespace ns(*this, entry.key, entry.value);
      actorNamespaces.insert(entry.key, kj::mv(ns));
    }
  }

  kj::Maybe<Service&> getEntrypoint(kj::StringPtr name) {
    return namedEntrypoints.find(name);
  }

  void link() override {
    LinkCallback callback = kj::mv(KJ_REQUIRE_NONNULL(
        ioChannels.tryGet<LinkCallback>(), "already called link()"));
    ioChannels = callback(*this);
  }

  kj::Maybe<ActorNamespace&> getActorNamespace(kj::StringPtr name) {
    return actorNamespaces.find(name);
  }

  kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata) override {
    return startRequest(kj::mv(metadata), nullptr);
  }

  kj::Own<WorkerInterface> startRequest(
      IoChannelFactory::SubrequestMetadata metadata, kj::Maybe<kj::StringPtr> entrypointName,
      kj::Maybe<kj::Own<Worker::Actor>> actor = nullptr) {
    return WorkerEntrypoint::construct(
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
        nullptr,                   // workerTracer
        kj::mv(metadata.cfBlobJson));
  }

  class ActorNamespace {
  public:
    ActorNamespace(WorkerService& service, kj::StringPtr className, const ActorConfig& config)
        : service(service), className(className), config(config) {}

    const ActorConfig& getConfig() { return config; }

    kj::Own<IoChannelFactory::ActorChannel> getActor(Worker::Actor::Id id) {
      // `getActor()` is often called with the calling isolate's lock held. We need to drop that
      // lock and take a lock on the target isolate before constructing the actor. Even if these
      // are the same isolate (as is commonly the case), we really don't want to do this stuff
      // synchronously, so this has the effect of pushing off to a later turn of the event loop.
      auto promise = service.worker->takeAsyncLockWithoutRequest(nullptr)
          .then([this, id = kj::mv(id)](Worker::AsyncLock lock) mutable -> kj::Own<ActorChannel> {
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

        auto actor = kj::addRef(*actors.findOrCreate(idStr, [&]() {
          auto persistent = config.tryGet<Durable>().map([&](const Durable& d) {
            // TODO(someday): Implement some sort of actual durable storage. For now we force
            //   `ActorCache` into `neverFlush` mode so that all state is kept in-memory.
            return rpc::ActorStorage::Stage::Client(kj::heap<EmptyReadOnlyActorStorageImpl>());
          });

          auto makeStorage = [](jsg::Lock& js, const Worker::ApiIsolate& apiIsolate,
                                ActorCache& actorCache)
                            -> jsg::Ref<api::DurableObjectStorage> {
            return jsg::alloc<api::DurableObjectStorage>(IoContext::current().addObject(actorCache));
          };

          TimerChannel& timerChannel = service;
          auto newActor = kj::refcounted<Worker::Actor>(
              *service.worker, kj::mv(id), true, kj::mv(persistent),
              className, kj::mv(makeStorage), lock,
              timerChannel, kj::refcounted<ActorObserver>());

          return kj::HashMap<kj::String, kj::Own<Worker::Actor>>::Entry {
            kj::mv(idStr), kj::mv(newActor)
          };
        }));

        return kj::heap<ActorChannelImpl>(service, className, kj::mv(actor));
      });

      return kj::heap<PromisedActorChannel>(service.waitUntilTasks, kj::mv(promise));
    }

  private:
    WorkerService& service;
    kj::StringPtr className;
    const ActorConfig& config;
    kj::HashMap<kj::String, kj::Own<Worker::Actor>> actors;
  };

private:
  class EntrypointService final: public Service {
  public:
    EntrypointService(WorkerService& worker, kj::StringPtr entrypoint)
        : worker(worker), entrypoint(entrypoint) {}

    kj::Own<WorkerInterface> startRequest(
        IoChannelFactory::SubrequestMetadata metadata) override {
      return worker.startRequest(kj::mv(metadata), entrypoint);
    }

  private:
    WorkerService& worker;
    kj::StringPtr entrypoint;
  };

  ThreadContext& threadContext;
  kj::Own<const Worker> worker;
  kj::HashMap<kj::String, EntrypointService> namedEntrypoints;
  kj::HashMap<kj::StringPtr, ActorNamespace> actorNamespaces;
  kj::OneOf<LinkCallback, LinkedIoChannels> ioChannels;
  kj::TaskSet waitUntilTasks;

  class ActorChannelImpl final: public IoChannelFactory::ActorChannel {
  public:
    ActorChannelImpl(WorkerService& service, kj::StringPtr className, kj::Own<Worker::Actor> actor)
        : service(service), className(className), actor(kj::mv(actor)) {}

    kj::Own<WorkerInterface> startRequest(
        IoChannelFactory::SubrequestMetadata metadata) override {
      return service.startRequest(kj::mv(metadata), className, kj::addRef(*actor));
    }

  private:
    WorkerService& service;
    kj::StringPtr className;
    kj::Own<Worker::Actor> actor;
  };

  class PromisedActorChannel final: public IoChannelFactory::ActorChannel {
  public:
    PromisedActorChannel(kj::TaskSet& waitUntilTasks, kj::Promise<kj::Own<ActorChannel>> promise)
        : waitUntilTasks(waitUntilTasks),
          promise(promise.then([this](kj::Own<ActorChannel> result) {
            channel = kj::mv(result);
          }).fork()) {}

    kj::Own<WorkerInterface> startRequest(
        IoChannelFactory::SubrequestMetadata metadata) override {
      KJ_IF_MAYBE(c, channel) {
        return c->get()->startRequest(kj::mv(metadata));
      } else {
        return newPromisedWorkerInterface(waitUntilTasks,
            promise.addBranch().then([this, metadata = kj::mv(metadata)]() mutable {
          return KJ_ASSERT_NONNULL(channel)->startRequest(kj::mv(metadata));
        }));
      }
    }

  private:
    kj::TaskSet& waitUntilTasks;
    kj::ForkedPromise<void> promise;
    kj::Maybe<kj::Own<ActorChannel>> channel;
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
                                       kj::Maybe<Tracer::Span&> parentSpan) override {

      return kj::heap<CacheHttpClientImpl>(
          cacheService, cacheNamespaceHeader, nullptr, kj::mv(cfBlobJson), kj::mv(parentSpan));
    }

    kj::Own<kj::HttpClient> getNamespace(kj::StringPtr cacheName,
                                         kj::Maybe<kj::String> cfBlobJson,
                                         kj::Maybe<Tracer::Span&> parentSpan) override {
      auto encodedName = kj::encodeUriComponent(cacheName);
      return kj::heap<CacheHttpClientImpl>(
          cacheService, cacheNamespaceHeader, kj::mv(encodedName), kj::mv(cfBlobJson), parentSpan);
    }

  private:
    Service& cacheService;
    kj::HttpHeaderId cacheNamespaceHeader;

  };

  class CacheHttpClientImpl final: public kj::HttpClient {
  public:
    CacheHttpClientImpl(Service& parent, kj::HttpHeaderId cacheNamespaceHeader,
                        kj::Maybe<kj::String> cacheName, kj::Maybe<kj::String> cfBlobJson,
                        kj::Maybe<Tracer::Span&> parentSpan)
        : client(asHttpClient(parent.startRequest({kj::mv(cfBlobJson), parentSpan}))),
          cacheName(kj::mv(cacheName)),
          cacheNamespaceHeader(cacheNamespaceHeader) {}

    Request request(kj::HttpMethod method, kj::StringPtr url,
                    const kj::HttpHeaders &headers,
                    kj::Maybe<uint64_t> expectedBodySize = nullptr) override {

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
      KJ_IF_MAYBE (name, cacheName) {
        headersCopy.set(cacheNamespaceHeader, *name);
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
    KJ_FAIL_REQUIRE("no logging channels");
  }

  kj::Own<ActorChannel> getGlobalActor(uint channel, const ActorIdFactory::ActorId& id,
      kj::Maybe<kj::String> locationHint) override {
    auto& channels = KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(),
        "link() has not been called");

    KJ_REQUIRE(channel < channels.actor.size(), "invalid actor channel number");
    auto& ns = JSG_REQUIRE_NONNULL(channels.actor[channel], Error,
        "Actor namespace configuration was invalid.");
    KJ_REQUIRE(ns.getConfig().is<Durable>());  // should have been verified earlier
    return ns.getActor(id.clone());
  }

  kj::Own<ActorChannel> getColoLocalActor(uint channel, kj::String id) override {
    auto& channels = KJ_REQUIRE_NONNULL(ioChannels.tryGet<LinkedIoChannels>(),
        "link() has not been called");

    KJ_REQUIRE(channel < channels.actor.size(), "invalid actor channel number");
    auto& ns = JSG_REQUIRE_NONNULL(channels.actor[channel], Error,
        "Actor namespace configuration was invalid.");
    KJ_REQUIRE(ns.getConfig().is<Ephemeral>());  // should have been verified earlier
    return ns.getActor(kj::str(id));
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

  kj::Own<void> enterJs(jsg::Lock& lock) override { return {}; }
  void topUpActor() override {}
  void newSubrequest(bool isInHouse) override {}
  void newKvRequest(KvOpType op) override {}
  void newAnalyticsEngineRequest() override {}
  kj::Promise<void> limitDrain() override { return kj::NEVER_DONE; }
  kj::Promise<void> limitScheduled() override { return kj::NEVER_DONE; }
  size_t getBufferingLimit() override { return kj::maxValue; }
  kj::Maybe<EventOutcome> getLimitsExceeded() override { return nullptr; }
  kj::Promise<void> onLimitsExceeded() override { return kj::NEVER_DONE; }
  void requireLimitsNotExceeded() override {}
  void reportMetrics(RequestObserver& requestMetrics) override {}
};

kj::Own<Server::Service> Server::makeWorker(kj::StringPtr name, config::Worker::Reader conf) {
  auto& localActorConfigs = KJ_ASSERT_NONNULL(actorConfigs.find(name));

  struct ErrorReporter: public Worker::ValidationErrorReporter {
    ErrorReporter(Server& server, kj::StringPtr name): server(server), name(name) {}

    Server& server;
    kj::StringPtr name;

    kj::HashSet<kj::String> namedEntrypoints;
    bool hasDefaultEntrypoint = false;

    void addError(kj::String error) override {
      server.reportConfigError(kj::str("service ", name, ": ", error));
    }

    void addHandler(kj::Maybe<kj::StringPtr> exportName, kj::StringPtr type) override {
      KJ_IF_MAYBE(e, exportName) {
        namedEntrypoints.findOrCreate(*e, [&]() { return kj::str(*e); });
      } else {
        hasDefaultEntrypoint = true;
      }
    }
  };

  ErrorReporter errorReporter(*this, name);

  capnp::MallocMessageBuilder arena;
  // TODO(beta): Factor out FeatureFlags from WorkerBundle.
  auto featureFlags = arena.initRoot<CompatibilityFlags>();

  if (conf.hasCompatibilityDate()) {
    compileCompatibilityFlags(conf.getCompatibilityDate(), conf.getCompatibilityFlags(),
                              featureFlags, errorReporter,
                              experimental
                                  ? CompatibilityDateValidation::CODE_VERSION_EXPERIMENTAL
                                  : CompatibilityDateValidation::CODE_VERSION);
  } else {
    errorReporter.addError(kj::str("Worker must specify compatibiltyDate."));
  }

  class NullIsolateLimitEnforcer final: public IsolateLimitEnforcer {
    // IsolateLimitEnforcer that enforces no limits.
  public:
    v8::Isolate::CreateParams getCreateParams() override { return {}; }
    void customizeIsolate(v8::Isolate* isolate) override {}
    ActorCacheSharedLruOptions getActorCacheLruOptions() override {
      // TODO(someday): Make this configurable?
      return {
        .softLimit = 16ull << 20,
        .hardLimit = 128ull << 20,
        .staleTimeout = 30 * kj::SECONDS,
        .dirtyKeySoftLimit = 64,
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
  };

  auto limitEnforcer = kj::heap<NullIsolateLimitEnforcer>();
  auto api = kj::heap<WorkerdApiIsolate>(globalContext->v8System,
      featureFlags.asReader(), *limitEnforcer);
  auto isolate = kj::atomicRefcounted<Worker::Isolate>(
      kj::mv(api),
      kj::atomicRefcounted<IsolateObserver>(),
      name,
      kj::mv(limitEnforcer),
      maybeInspectorService != nullptr);

  // If we are using the inspector, we need to register the Worker::Isolate
  // with the inspector service.
  KJ_IF_MAYBE(inspector, maybeInspectorService) {
    (*inspector)->registerIsolate(name, isolate.get());
  }

  auto script = isolate->newScript(name, WorkerdApiIsolate::extractSource(conf, errorReporter),
                                   IsolateObserver::StartType::COLD,
                                   false, errorReporter);

  struct FutureSubrequestChannel {
    config::ServiceDesignator::Reader designator;
    kj::String errorContext;
  };
  kj::Vector<FutureSubrequestChannel> subrequestChannels;

  struct FutureActorChannel {
    config::Worker::Binding::DurableObjectNamespaceDesignator::Reader designator;
    kj::String errorContext;
  };
  kj::Vector<FutureActorChannel> actorChannels;

  auto confBindings = conf.getBindings();
  using Global = WorkerdApiIsolate::Global;
  kj::Vector<Global> globals(confBindings.size());
  for (auto binding: confBindings) {
    kj::StringPtr bindingName = binding.getName();
    auto addGlobal = [&](auto&& value) {
      globals.add(Global {
        .name = kj::str(bindingName),
        .value = kj::mv(value)
      });
    };

    auto errorContext = kj::str("Worker \"", name , "\"'s binding \"", bindingName, "\"");

    switch (binding.which()) {
      case config::Worker::Binding::UNSPECIFIED:
        errorReporter.addError(kj::str(errorContext, " does not specify any binding value."));
        continue;

      case config::Worker::Binding::PARAMETER:
        KJ_UNIMPLEMENTED("TODO(beta): parameters");

      case config::Worker::Binding::TEXT:
        addGlobal(kj::str(binding.getText()));
        continue;
      case config::Worker::Binding::DATA:
        addGlobal(kj::heapArray<byte>(binding.getData()));
        continue;
      case config::Worker::Binding::JSON:
        addGlobal(Global::Json { kj::str(binding.getJson()) });
        continue;

      case config::Worker::Binding::WASM_MODULE:
        if (conf.isServiceWorkerScript()) {
          // Already handled earlier.
        } else {
          errorReporter.addError(kj::str(
              errorContext, " is a Wasm binding, but Wasm bindings are not allowed in "
              "modules-based scripts. Use Wasm modules instead."));
        }
        continue;

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
              continue;
            });
            if (pem.type != "PRIVATE KEY") {
              errorReporter.addError(kj::str(
                  "CryptoKey binding \"", binding.getName(), "\" contained wrong PEM type, "
                  "expected \"PRIVATE KEY\" but got \"", pem.type, "\"."));
              continue;
            }
            keyGlobal.keyData = kj::mv(pem.data);
            goto validFormat;
          }
          case config::Worker::Binding::CryptoKey::SPKI: {
            keyGlobal.format = kj::str("spki");
            auto pem = KJ_UNWRAP_OR(decodePem(keyConf.getSpki()), {
              errorReporter.addError(kj::str(
                  "CryptoKey binding \"", binding.getName(), "\" contained invalid PEM format."));
              continue;
            });
            if (pem.type != "PUBLIC KEY") {
              errorReporter.addError(kj::str(
                  "CryptoKey binding \"", binding.getName(), "\" contained wrong PEM type, "
                  "expected \"PUBLIC KEY\" but got \"", pem.type, "\"."));
              continue;
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
        continue;
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
        continue;
      validAlgorithm:

        keyGlobal.extractable = keyConf.getExtractable();
        keyGlobal.usages = KJ_MAP(usage, keyConf.getUsages()) { return kj::str(usage); };

        addGlobal(kj::mv(keyGlobal));
        continue;
      }

      case config::Worker::Binding::SERVICE: {
        addGlobal(Global::Fetcher {
          .channel = (uint)subrequestChannels.size() +
              IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT,
          .requiresHost = true,
          .isInHouse = false
        });

        subrequestChannels.add(FutureSubrequestChannel {
          binding.getService(),
          kj::mv(errorContext)
        });
        continue;
      }

      case config::Worker::Binding::DURABLE_OBJECT_NAMESPACE: {
        auto actorBinding = binding.getDurableObjectNamespace();
        const ActorConfig* actorConfig;
        if (actorBinding.hasServiceName()) {
          auto& svcMap = KJ_UNWRAP_OR(actorConfigs.find(actorBinding.getServiceName()), {
            errorReporter.addError(kj::str(
                errorContext, " refers to a service \"", actorBinding.getServiceName(),
                "\", but no such service is defined."));
            continue;
          });

          actorConfig = &KJ_UNWRAP_OR(svcMap.find(actorBinding.getClassName()), {
            errorReporter.addError(kj::str(
                errorContext, " refers to a Durable Object namespace named \"",
                actorBinding.getClassName(), "\" in service \"", actorBinding.getServiceName(),
                "\", but no such Durable Object namespace is defined by that service."));
            continue;
          });
        } else {
          actorConfig = &KJ_UNWRAP_OR(localActorConfigs.find(actorBinding.getClassName()), {
            errorReporter.addError(kj::str(
                errorContext, " refers to a Durable Object namespace named \"",
                actorBinding.getClassName(), "\", but no such Durable Object namespace is defined "
                "by this Worker."));
            continue;
          });
        }

        KJ_SWITCH_ONEOF(*actorConfig) {
          KJ_CASE_ONEOF(durable, Durable) {
            addGlobal(Global::DurableActorNamespace {
              .actorChannel = (uint)actorChannels.size(),
              .uniqueKey = durable.uniqueKey
            });
          }
          KJ_CASE_ONEOF(_, Ephemeral) {
            addGlobal(Global::EphemeralActorNamespace {
              .actorChannel = (uint)actorChannels.size(),
            });
          }
        }

        actorChannels.add(FutureActorChannel {
          actorBinding,
          kj::mv(errorContext)
        });
        continue;
      }

      case config::Worker::Binding::KV_NAMESPACE: {
        addGlobal(Global::KvNamespace {
          .subrequestChannel = (uint)subrequestChannels.size() +
              IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT
        });

        subrequestChannels.add(FutureSubrequestChannel {
          binding.getKvNamespace(),
          kj::mv(errorContext)
        });
        continue;
      }

      case config::Worker::Binding::R2_BUCKET: {
        addGlobal(Global::R2Bucket {
          .subrequestChannel = (uint)subrequestChannels.size() +
              IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT
        });

        subrequestChannels.add(FutureSubrequestChannel {
          binding.getR2Bucket(),
          kj::mv(errorContext)
        });
        continue;
      }

      case config::Worker::Binding::R2_ADMIN: {
        addGlobal(Global::R2Admin {
          .subrequestChannel = (uint)subrequestChannels.size() +
              IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT
        });

        subrequestChannels.add(FutureSubrequestChannel {
          binding.getR2Admin(),
          kj::mv(errorContext)
        });
        continue;
      }
    }
    errorReporter.addError(kj::str(
        errorContext, "has unrecognized type. Was the config compiled with a newer version of "
        "the schema?"));
  }

  auto worker = kj::atomicRefcounted<Worker>(
      kj::mv(script),
      kj::atomicRefcounted<WorkerObserver>(),
      [&](jsg::Lock& lock, const Worker::ApiIsolate& apiIsolate, v8::Local<v8::Object> target) {
        return kj::downcast<const WorkerdApiIsolate>(apiIsolate).compileGlobals(
            lock, globals, target, 1);
      },
      IsolateObserver::StartType::COLD,
      nullptr,          // systemTracer -- TODO(beta): factor out
      Worker::Lock::TakeSynchronously(nullptr),
      errorReporter);

  {
    Worker::Lock lock(*worker, Worker::Lock::TakeSynchronously(nullptr));
    lock.validateHandlers(errorReporter);
  }

  auto linkCallback =
      [this, name, conf, subrequestChannels = kj::mv(subrequestChannels),
       actorChannels = kj::mv(actorChannels)](WorkerService& workerService) mutable {
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

    auto actors = KJ_MAP(channel, actorChannels) -> kj::Maybe<WorkerService::ActorNamespace&> {
      WorkerService* targetService = &workerService;
      if (channel.designator.hasServiceName()) {
        auto& svc = KJ_UNWRAP_OR(this->services.find(channel.designator.getServiceName()), {
          // error was reported earlier
          return nullptr;
        });
        targetService = dynamic_cast<WorkerService*>(svc.get());
        if (targetService == nullptr) {
          // error was reported earlier
          return nullptr;
        }
      }

      // (If getActorNamespace() returns null, an error was reported earlier.)
      return targetService->getActorNamespace(channel.designator.getClassName());
    };

    if (conf.hasCacheApiOutbound()) {
      Service& cacheApi = lookupService(conf.getCacheApiOutbound(),
                                        kj::str("Worker \"", name, "\"'s cacheApiOutbound"));

      return WorkerService::LinkedIoChannels{
          .subrequest = services.finish(),
          .actor = kj::mv(actors),
          .cache = &cacheApi
      };
    } else {
      return WorkerService::LinkedIoChannels{
          .subrequest = services.finish(),
          .actor = kj::mv(actors)
      };
    }

  };

  return kj::heap<WorkerService>(globalContext->threadContext, kj::mv(worker),
                                 kj::mv(errorReporter.namedEntrypoints), localActorConfigs,
                                 kj::mv(linkCallback));
}

// =======================================================================================

kj::Own<Server::Service> Server::makeService(
    config::Service::Reader conf,
    kj::HttpHeaderTable::Builder& headerTableBuilder) {
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
      return makeWorker(name, conf.getWorker());

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
      KJ_IF_MAYBE(ep, worker->getEntrypoint(entrypointName)) {
        return *ep;
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

class Server::HttpListener final: private kj::TaskSet::ErrorHandler {
public:
  HttpListener(kj::Own<kj::ConnectionReceiver> listener, Service& service,
              kj::StringPtr physicalProtocol, kj::Own<HttpRewriter> rewriter,
               kj::HttpHeaderTable& headerTable, kj::Timer& timer)
      : listener(kj::mv(listener)), service(service),
        headerTable(headerTable), timer(timer),
        physicalProtocol(physicalProtocol),
        rewriter(kj::mv(rewriter)),
        tasks(*this) {}

  kj::Promise<void> run() {
    return listener->acceptAuthenticated()
        .then([this](kj::AuthenticatedStream stream) {
      kj::Maybe<kj::String> cfBlobJson;
      if (!rewriter->hasCfBlobHeader()) {
        // Construct a cf blob describing the client identity.

        kj::PeerIdentity* peerId;

        KJ_IF_MAYBE(tlsId,
            kj::dynamicDowncastIfAvailable<kj::TlsPeerIdentity>(*stream.peerIdentity)) {
          peerId = &tlsId->getNetworkIdentity();

          // TODO(someday): Add client certificate info to the cf blob? At present, KJ only
          //   supplies the common name, but that doesn't even seem to be one of the fields that
          //   Cloudflare-hosted Workers receive. We should probably try to match those.
        } else {
          peerId = stream.peerIdentity;
        }

        KJ_IF_MAYBE(remote,
            kj::dynamicDowncastIfAvailable<kj::NetworkPeerIdentity>(*peerId)) {
          cfBlobJson = kj::str("{\"clientIp\": \"", escapeJsonString(remote->toString()), "\"}");
        } else KJ_IF_MAYBE(local,
            kj::dynamicDowncastIfAvailable<kj::LocalPeerIdentity>(*peerId)) {
          auto creds = local->getCredentials();

          kj::Vector<kj::String> parts;
          KJ_IF_MAYBE(p, creds.pid) {
            parts.add(kj::str("\"clientPid\":", *p));
          }
          KJ_IF_MAYBE(u, creds.uid) {
            parts.add(kj::str("\"clientUid\":", *u));
          }

          cfBlobJson = kj::str("{", kj::strArray(parts, ","), "}");
        }
      }

      auto conn = kj::heap<Connection>(*this, kj::mv(cfBlobJson));
      tasks.add(conn->http.listenHttp(kj::mv(stream.stream)).attach(kj::mv(conn)));

      return run();
    });
  }

private:
  kj::Own<kj::ConnectionReceiver> listener;
  Service& service;
  kj::HttpHeaderTable& headerTable;
  kj::Timer& timer;
  kj::StringPtr physicalProtocol;
  kj::Own<HttpRewriter> rewriter;
  kj::TaskSet tasks;

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, exception);
  }

  struct Connection final: public kj::HttpService, public kj::HttpServerErrorHandler {
    Connection(HttpListener& parent, kj::Maybe<kj::String> cfBlobJson)
        : parent(parent), cfBlobJson(kj::mv(cfBlobJson)),
          http(parent.timer, parent.headerTable, *this, kj::HttpServerSettings {
            .errorHandler = *this,
            .webSocketCompressionMode = kj::HttpServerSettings::MANUAL_COMPRESSION
          }) {}

    HttpListener& parent;
    kj::Maybe<kj::String> cfBlobJson;
    kj::HttpServer http;

    class ResponseWrapper final: public kj::HttpService::Response {
    public:
      ResponseWrapper(kj::HttpService::Response& inner, HttpRewriter& rewriter)
          : inner(inner), rewriter(rewriter) {}

      kj::Own<kj::AsyncOutputStream> send(
          uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
          kj::Maybe<uint64_t> expectedBodySize = nullptr) override {
        auto rewrite = headers.cloneShallow();
        rewriter.rewriteResponse(rewrite);
        return inner.send(statusCode, statusText, rewrite, expectedBodySize);
      }

      kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
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
      IoChannelFactory::SubrequestMetadata metadata;
      metadata.cfBlobJson = cfBlobJson.map([](kj::StringPtr s) { return kj::str(s); });

      Response* wrappedResponse = &response;
      kj::Own<ResponseWrapper> ownResponse;
      if (parent.rewriter->needsRewriteResponse()) {
        wrappedResponse = ownResponse = kj::heap<ResponseWrapper>(response, *parent.rewriter);
      }

      if (parent.rewriter->needsRewriteRequest() || cfBlobJson != nullptr) {
        auto rewrite = KJ_UNWRAP_OR(
            parent.rewriter->rewriteIncomingRequest(
                url, parent.physicalProtocol, headers, metadata.cfBlobJson), {
          return response.sendError(400, "Bad Request", parent.headerTable);
        });
        auto worker = parent.service.startRequest(kj::mv(metadata));
        return worker->request(method, url, *rewrite.headers, requestBody, *wrappedResponse)
            .attach(kj::mv(rewrite), kj::mv(worker), kj::mv(ownResponse));
      } else {
        auto worker = parent.service.startRequest(kj::mv(metadata));
        return worker->request(method, url, headers, requestBody, *wrappedResponse)
            .attach(kj::mv(worker), kj::mv(ownResponse));
      }
    }

    // ---------------------------------------------------------------------------
    // implements kj::HttpServerErrorHandler

    kj::Promise<void> handleApplicationError(
        kj::Exception exception, kj::Maybe<kj::HttpService::Response&> response) override {
      KJ_LOG(ERROR, kj::str("Uncaught exception: ", exception));
      KJ_IF_MAYBE(r, response) {
        return r->sendError(500, "Internal Server Error", parent.headerTable);
      } else {
        return kj::READY_NOW;
      }
    }
  };
};

kj::Promise<void> Server::listenHttp(
    kj::Own<kj::ConnectionReceiver> listener, Service& service,
    kj::StringPtr physicalProtocol, kj::Own<HttpRewriter> rewriter) {
  auto obj = kj::heap<HttpListener>(kj::mv(listener), service,
                                    physicalProtocol, kj::mv(rewriter),
                                    globalContext->headerTable, timer);
  return obj->run().attach(kj::mv(obj));
}

// =======================================================================================

kj::Promise<void> Server::run(jsg::V8System& v8System, config::Config::Reader config) {
  kj::HttpHeaderTable::Builder headerTableBuilder;
  globalContext = kj::heap<GlobalContext>(*this, v8System, headerTableBuilder);
  invalidConfigServiceSingleton = kj::heap<InvalidConfigService>();

  auto [ fatalPromise, fatalFulfiller ] = kj::newPromiseAndFulfiller<void>();
  this->fatalFulfiller = kj::mv(fatalFulfiller);

  // ---------------------------------------------------------------------------
  // Configure inspector.

  KJ_IF_MAYBE(inspector, inspectorOverride) {
    // Create the special inspector service.
    maybeInspectorService = makeInspectorService(headerTableBuilder);
    auto& inspectorService = *KJ_ASSERT_NONNULL(maybeInspectorService);

    // Configure and start the inspector socket.
    static constexpr uint DEFAULT_PORT = 9229;

    auto inspectorListener = network.parseAddress(*inspector, DEFAULT_PORT)
        .then([](kj::Own<kj::NetworkAddress> parsed) {
      return parsed->listen();
    });

    tasks.add(inspectorListener.then(
        [&inspectorService](kj::Own<kj::ConnectionReceiver> listener) mutable {
      KJ_LOG(INFO, "Inspector is listening");
      return inspectorService.listen(kj::mv(listener));
    }));
  }

  // ---------------------------------------------------------------------------
  // Configure services

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
                Durable { kj::str(ns.getUniqueKey()) });
            continue;
          case config::Worker::DurableObjectNamespace::EPHEMERAL_LOCAL:
            if (!experimental) {
              reportConfigError(kj::str(
                  "Ephemeral objects (Durable Object namespaces with type 'ehpmeralLocal') are an "
                  "experimental feature which may change or go away in the future. You must run "
                  "workerd with `--experimental` to use this feature."));
            }
            serviceActorConfigs.insert(kj::str(ns.getClassName()), Ephemeral {});
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

  // Second pass: Build services.
  for (auto serviceConf: config.getServices()) {
    kj::StringPtr name = serviceConf.getName();
    auto service = makeService(serviceConf, headerTableBuilder);

    services.upsert(kj::str(name), kj::mv(service), [&](auto&&...) {
      reportConfigError(kj::str("Config defines multiple services named \"", name, "\"."));
    });
  }

  // Make the default "internet" service if it's not there already.
  services.findOrCreate("internet"_kj, [&]() {
    auto publicNetwork = network.restrictPeers({"public"_kj});

    kj::TlsContext::Options options;
    options.useSystemTrustStore = true;

    auto tls = kj::heap<kj::TlsContext>(kj::mv(options));
    auto tlsNetwork = tls->wrapNetwork(*publicNetwork).attach(kj::mv(tls));

    auto service = kj::heap<NetworkService>(
        globalContext->headerTable, timer, entropySource,
        kj::mv(publicNetwork), kj::mv(tlsNetwork));

    return decltype(services)::Entry {
      kj::str("internet"_kj),
      kj::mv(service)
    };
  });

  // Third pass: Cross-link services.
  for (auto& service: services) {
    service.value->link();
  }

  // ---------------------------------------------------------------------------
  // Start sockets

  for (auto sock: config.getSockets()) {
    kj::StringPtr name = sock.getName();
    kj::StringPtr addrStr = nullptr;
    kj::String ownAddrStr;
    kj::Maybe<kj::Own<kj::ConnectionReceiver>> listenerOverride;

    Service& service = lookupService(sock.getService(), kj::str("Socket \"", name, "\""));

    KJ_IF_MAYBE(override, socketOverrides.findEntry(name)) {
      KJ_SWITCH_ONEOF(override->value) {
        KJ_CASE_ONEOF(str, kj::String) {
          addrStr = ownAddrStr = kj::mv(str);
          break;
        }
        KJ_CASE_ONEOF(l, kj::Own<kj::ConnectionReceiver>) {
          listenerOverride = kj::mv(l);
          break;
        }
      }
      socketOverrides.erase(*override);
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
    kj::Promise<kj::Own<kj::ConnectionReceiver>> listener = nullptr;
    KJ_IF_MAYBE(l, listenerOverride) {
      listener = kj::mv(*l);
    } else {
      listener = network.parseAddress(addrStr, defaultPort)
          .then([](kj::Own<kj::NetworkAddress> parsed) {
        return parsed->listen();
      });
    }

    KJ_IF_MAYBE(t, tls) {
      listener = listener.then([tls = kj::mv(*t)](kj::Own<kj::ConnectionReceiver> port) mutable {
        return tls->wrapPort(kj::mv(port)).attach(kj::mv(tls));
      });
    }

    // Need to create rewriter before waiting on anything since `headerTableBuilder` will no longer
    // be available later.
    auto rewriter = kj::heap<HttpRewriter>(httpOptions, headerTableBuilder);

    tasks.add(listener
        .then([this, &service, rewriter = kj::mv(rewriter), physicalProtocol]
              (kj::Own<kj::ConnectionReceiver> listener) mutable {
      return listenHttp(kj::mv(listener), service, physicalProtocol, kj::mv(rewriter));
    }));
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
    reportConfigError(kj::str(
        "Config did not define any disk service named \"", unmatched.key, "\" to match the "
        "override provided on the command line."));
  }

  // We should have registered all headers synchronously. This is important becaues we want to
  // be able to start handling requests as soon as the services are available, even if some other
  // services take longer to get ready.
  auto ownHeaderTable = headerTableBuilder.build();

  // Wait until startup tasks finish. Note we may start serving requests on some services in
  // the meantime.
  return tasks.onEmpty().exclusiveJoin(kj::mv(fatalPromise)).attach(kj::mv(ownHeaderTable));
}

}  // namespace workerd::server
