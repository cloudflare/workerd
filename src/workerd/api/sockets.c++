// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sockets.h"
#include "system-streams.h"
#include <workerd/util/thread-scopes.h>


namespace workerd::api {


bool isValidHost(kj::StringPtr host) {
  // This function performs some basic length and characters checks, it does not guarantee that
  // the specified host is a valid domain. It should only be used to reject malicious
  // hosts.
  if (host.size() > 255 || host.size() == 0) {
    // RFC1035 states that maximum domain name length is 255 octets.
    //
    // IP addresses are always shorter, so we take the max domain length instead.
    return false;
  }

  for (int i = 0; i < host.size(); i++) {
    switch (host[i]) {
      case '-':
      case '.':
      case '_':
      case '[': case ']': case ':': // For IPv6.
        break;
      default:
        if ((host[i] >= 'a' && host[i] <= 'z') ||
            (host[i] >= 'A' && host[i] <= 'Z') ||
            (host[i] >= '0' && host[i] <= '9')) {
          break;
        }
        return false;
    }
  }
  return true;
}

kj::Maybe<kj::String> getHost(kj::StringPtr address) {
  // Simple address parser which extracts the hostname. Intentionally simple as its likely we will
  // not need this in the long-term (if we decide to change `connect` api to take host/port
  // separately).
  auto portIndex = address.findFirst(':');
  if (portIndex == nullptr) {
    return kj::str(address);
  }
  if (portIndex != address.findLast(':')) {
    return nullptr;
  }

  return kj::str(address.slice(0, KJ_ASSERT_NONNULL(portIndex)));
}

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options) {

  auto addressStr = kj::str("");
  KJ_SWITCH_ONEOF(address) {
    KJ_CASE_ONEOF(str, kj::String) {
      addressStr = kj::mv(str);
    }
    KJ_CASE_ONEOF(record, SocketAddress) {
      addressStr = kj::str(record.hostname, ":", record.port);
    }
  }

  JSG_REQUIRE(isValidHost(addressStr), TypeError,
      "Specified address is empty string, contains unsupported characters or is too long.");

  auto& ioContext = IoContext::current();

  auto jsRequest = Request::constructor(js, kj::str(addressStr), nullptr);
  kj::Own<WorkerInterface> client = fetcher->getClient(
      ioContext, jsRequest->serializeCfBlobJson(js), "connect"_kj);

  // Set up the connection.
  auto headers = kj::heap<kj::HttpHeaders>(ioContext.getHeaderTable());
  auto httpClient = kj::newHttpClient(*client);
  auto request = httpClient->connect(addressStr, *headers);
  auto socketStream = kj::mv(request.connection);
  kj::Maybe<kj::Array<kj::TlsCertificate>> systemCerts;
  KJ_IF_MAYBE(opts, options) {
    if (opts->tls) {
      // Configure a default TLS context.
      kj::TlsContext::Options options;
      KJ_IF_MAYBE(certs, getGlobalTlsSystemCerts()) {
        // We have a custom list of certificates to use. Parse them and use them instead of
        // the system trust store. This is used upstream.
        options.useSystemTrustStore = false;
        systemCerts = kj::parseCertificatesList(*certs);
        KJ_REQUIRE(KJ_ASSERT_NONNULL(systemCerts).size() > 0, "No certificates parsed");
        options.trustedCertificates = KJ_ASSERT_NONNULL(systemCerts).asPtr();
      } else {
        options.useSystemTrustStore = true;
      }

      auto tls = kj::heap<kj::TlsContext>(kj::mv(options));
      auto maybeHost = getHost(addressStr);
      // TODO(later): Allow custom host to be specified in Socket options.
      auto host = kj::mv(JSG_REQUIRE_NONNULL(maybeHost, TypeError,
          "Specified address has no hostname, cannot establish TLS connection."));
      // Wrap the socket stream in the TLS context.
      socketStream = kj::newPromisedStream(
          tls->wrapClient(kj::mv(socketStream), host).attach(kj::mv(tls)));
    }
  }

  // Initialise the readable/writable streams with the readable/writable sides of an AsyncIoStream.
  auto sysStreams = newSystemMultiStream(kj::mv(socketStream), ioContext);
  auto readable = jsg::alloc<ReadableStream>(ioContext, kj::mv(sysStreams.readable));
  auto writable = jsg::alloc<WritableStream>(ioContext, kj::mv(sysStreams.writable));

  auto closeFulfiller = kj::heap<jsg::PromiseResolverPair<void>>(
      jsg::newPromiseAndResolver<void>(ioContext.getCurrentLock().getIsolate()));
  closeFulfiller->promise.markAsHandled();

  auto result = jsg::alloc<Socket>(
      js, kj::mv(readable), kj::mv(writable), kj::mv(closeFulfiller), kj::mv(systemCerts));
  // `handleProxyStatus` needs an initialised refcount to use `JSG_THIS`, hence it cannot be
  // called in Socket's constructor.
  result->handleProxyStatus(js, kj::mv(request.status));
  return result;
}

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options,
    CompatibilityFlags::Reader featureFlags) {
  // `connect()` should be hidden when the feature flag is off, so we shouldn't even get here.
  KJ_ASSERT(featureFlags.getTcpSocketsSupport());

  jsg::Ref<Fetcher> actualFetcher = nullptr;
  KJ_IF_MAYBE(f, fetcher) {
    actualFetcher = kj::mv(*f);
  } else {
    actualFetcher = jsg::alloc<Fetcher>(
        IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
  }
  return connectImplNoOutputLock(js, kj::mv(actualFetcher), kj::mv(address), kj::mv(options));
}

jsg::Promise<void> Socket::close(jsg::Lock& js) {
  // Forcibly close the readable/writable streams.
  auto cancelPromise = readable->getController().cancel(js, nullptr);
  auto abortPromise = writable->getController().abort(js, nullptr);
  // The below is effectively `Promise.all(cancelPromise, abortPromise)`
  return cancelPromise.then(js, [abortPromise = kj::mv(abortPromise), this](jsg::Lock& js) mutable {
    return abortPromise.then(js, [this](jsg::Lock& js) {
      resolveFulfiller(js, nullptr);
      return js.resolvedPromise();
    });
  }, [this](jsg::Lock& js, jsg::Value err) { return errorHandler(js, kj::mv(err)); });
}

void Socket::handleProxyStatus(
    jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status) {
  auto& context = IoContext::current();
  auto result = context.awaitIo(js, kj::mv(status),
      [this, self = JSG_THIS](jsg::Lock& js, kj::HttpClient::ConnectRequest::Status&& status) -> void {
    if (status.statusCode < 200 || status.statusCode >= 300) {
      // If the status indicates an unsucessful connection we need to reject the `closeFulfiller`
      // with an exception. This will reject the socket's `closed` promise.
      auto exc = kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
        kj::str(JSG_EXCEPTION(Error) ": proxy request failed"));
      resolveFulfiller(js, exc);
    }
  });
  result.markAsHandled();
}

}  // namespace workerd::api
