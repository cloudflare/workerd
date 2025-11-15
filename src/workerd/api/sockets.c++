// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sockets.h"

#include "streams/standard.h"
#include "system-streams.h"

#include <workerd/io/worker-interface.h>
#include <workerd/jsg/exception.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

namespace {

// This function performs some basic length and characters checks, it does not guarantee that
// the specified host is a valid domain. It should only be used to reject malicious
// hosts.
bool isValidHost(kj::StringPtr host) {
  if (host.size() > 255 || host.size() == 0) {
    // RFC1035 states that maximum domain name length is 255 octets.
    //
    // IP addresses are always shorter, so we take the max domain length instead.
    return false;
  }

  for (auto i: kj::indices(host)) {
    switch (host[i]) {
      case '-':
      case '.':
      case '_':
      case '[':
      case ']':
      case ':':  // For IPv6.
        break;
      default:
        if ((host[i] >= 'a' && host[i] <= 'z') || (host[i] >= 'A' && host[i] <= 'Z') ||
            (host[i] >= '0' && host[i] <= '9')) {
          break;
        }
        return false;
    }
  }
  return true;
}

SecureTransportKind parseSecureTransport(SocketOptions& opts) {
  auto value = KJ_UNWRAP_OR_RETURN(opts.secureTransport, SecureTransportKind::OFF).begin();
  if (value == "off"_kj) {
    return SecureTransportKind::OFF;
  } else if (value == "starttls"_kj) {
    return SecureTransportKind::STARTTLS;
  } else if (value == "on"_kj) {
    return SecureTransportKind::ON;
  } else {
    JSG_FAIL_REQUIRE(
        TypeError, kj::str("Unsupported value in secureTransport socket option: ", value));
  }
}

bool getAllowHalfOpen(jsg::Optional<SocketOptions>& opts) {
  KJ_IF_SOME(o, opts) {
    return o.allowHalfOpen;
  }

  // The allowHalfOpen flag is false by default.
  return false;
}

kj::Maybe<uint64_t> getWritableHighWaterMark(jsg::Optional<SocketOptions>& opts) {
  KJ_IF_SOME(o, opts) {
    return o.highWaterMark;
  }
  return kj::none;
}

}  // namespace

// Forward declarations
class StreamWorkerInterface;

jsg::Ref<Socket> setupSocket(jsg::Lock& js,
    kj::Own<kj::AsyncIoStream> connection,
    kj::String remoteAddress,
    jsg::Optional<SocketOptions> options,
    kj::Own<kj::TlsStarterCallback> tlsStarter,
    SecureTransportKind secureTransport,
    kj::String domain,
    bool isDefaultFetchPort,
    kj::Maybe<jsg::PromiseResolverPair<SocketInfo>> maybeOpenedPrPair) {
  auto& ioContext = IoContext::current();

  // Disconnection handling is annoyingly complicated:
  //
  // We can't just context.awaitIo(connection->whenWriteDisconnected()) directly, because the
  // Socket could be GC'ed before `whenWriteDisconnected()` completes, causing the underlying
  // `connection` to be destroyed. By KJ rules, we are required to cancel the promise returned by
  // `whenWriteDisconnected()` before destroying `connection`. But there's no way to cancel a
  // promise passed to `context.awaitIo()`. We have to hold the promise directly in `Socket`, so
  // that we can cancel it on destruction. But we *do* want to create a JS promise that resolves
  // on disconnect, which is what awaitIo() would give us.
  //
  // So, we have to chain through a promise/fulfiller pair. The `Socket` holds
  // `watchForDisconnectTask`, which is a `kj::Promise<void>` representing a task that waits for
  // `whenWriteDisconnected()` and then fulfills the fulfiller end of `disconnectedPaf` with
  // `false`. If the task is canceled, we instead fulfill `disconnectedPaf` with `true`.
  //
  // We then use `context.awaitIo()` to await the promise end of `disconnectedPaf`, and this gives
  // us our `closed` promise. Well, almost...
  //
  // There's another wrinkle: There are some circumstances where we want to resolve the `closed`
  // promise directly from an API call. We'd rather this did not have to drop out of the isolate
  // and enter it a gain. So, our `awaitIo()` actually awaits a task that listens for the
  // disconnected promise and then resolves some other JS resolver, `closedResolver`.
  auto disconnectedPaf = kj::newPromiseAndFulfiller<bool>();
  auto& disconnectedFulfiller = *disconnectedPaf.fulfiller;
  auto deferredCancelDisconnected =
      kj::defer([fulfiller = kj::mv(disconnectedPaf.fulfiller)]() mutable {
    // In case the `whenWriteDisconected()` listener task is canceled without fulfilling the
    // fulfiller, we want to silently fulfill it. This will happen when the Socket is GC'ed.
    fulfiller->fulfill(true);
  });

  static auto constexpr handleDisconnected =
      [](kj::AsyncIoStream& connection,
          kj::PromiseFulfiller<bool>& fulfiller) -> kj::Promise<void> {
    try {
      co_await connection.whenWriteDisconnected();
      fulfiller.fulfill(false);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      fulfiller.reject(kj::mv(exception));
    }
  };

  auto watchForDisconnectTask = handleDisconnected(*connection, disconnectedFulfiller)
                                    .attach(kj::mv(deferredCancelDisconnected));

  auto closedPrPair = js.newPromiseAndResolver<void>();
  closedPrPair.promise.markAsHandled(js);

  ioContext.awaitIo(js, kj::mv(disconnectedPaf.promise))
      .then(
          js, [resolver = closedPrPair.resolver.addRef(js)](jsg::Lock& js, bool canceled) mutable {
    // We want to silently ignore the canceled case, without ever resolving anything. Note that
    // if the application actually fetches the `closed` promise, then the JSG glue will prevent
    // the socket from being GC'ed until that promise resolves, so it won't be canceled.
    if (!canceled) {
      resolver.resolve(js);
    }
  }, [resolver = closedPrPair.resolver.addRef(js)](jsg::Lock& js, jsg::Value exception) mutable {
    resolver.reject(js, exception.getHandle(js));
  });

  auto refcountedConnection = kj::refcountedWrapper(kj::mv(connection));
  // Initialize the readable/writable streams with the readable/writable sides of an AsyncIoStream.
  auto sysStreams = newSystemMultiStream(refcountedConnection->addWrappedRef(), ioContext);
  auto readable = js.alloc<ReadableStream>(ioContext, kj::mv(sysStreams.readable));
  auto allowHalfOpen = getAllowHalfOpen(options);
  kj::Maybe<jsg::Promise<void>> eofPromise;
  if (!allowHalfOpen) {
    eofPromise = readable->onEof(js);
  }
  auto openedPrPair = kj::mv(maybeOpenedPrPair).orDefault(js.newPromiseAndResolver<SocketInfo>());
  openedPrPair.promise.markAsHandled(js);
  auto writable = js.alloc<WritableStream>(ioContext, kj::mv(sysStreams.writable),
      ioContext.getMetrics().tryCreateWritableByteStreamObserver(),
      getWritableHighWaterMark(options), openedPrPair.promise.whenResolved(js));

  auto result = js.alloc<Socket>(js, ioContext, kj::mv(refcountedConnection), kj::mv(remoteAddress),
      kj::mv(readable), kj::mv(writable), kj::mv(closedPrPair), kj::mv(watchForDisconnectTask),
      kj::mv(options), kj::mv(tlsStarter), secureTransport, kj::mv(domain), isDefaultFetchPort,
      kj::mv(openedPrPair));

  KJ_IF_SOME(p, eofPromise) {
    result->handleReadableEof(js, kj::mv(p));
  }
  return result;
}

jsg::Ref<Socket> connectImplNoOutputLock(jsg::Lock& js,
    kj::Maybe<jsg::Ref<Fetcher>> fetcher,
    AnySocketAddress address,
    jsg::Optional<SocketOptions> options) {

  auto& ioContext = IoContext::current();
  JSG_REQUIRE(!ioContext.isFiddle(), TypeError, "Socket API not supported in web preview mode.");

  // Extract the domain/ip we are connecting to from the address.
  kj::String domain;
  bool isDefaultFetchPort = false;

  KJ_SWITCH_ONEOF(address) {
    KJ_CASE_ONEOF(str, kj::String) {
      // We need just the hostname part of the address, i.e. we want to strip out the port.
      // We do this using the standard URL parser since it will handle IPv6 for us as well.
      auto input = kj::str("fake://", str);
      auto url = JSG_REQUIRE_NONNULL(
          jsg::Url::tryParse(input.asPtr()), TypeError, "Specified address could not be parsed.");
      auto host = url.getHostname();
      auto port = url.getPort();
      JSG_REQUIRE(host != ""_kj, TypeError, "Specified address is missing hostname.");
      JSG_REQUIRE(port != ""_kj, TypeError, "Specified address is missing port.");
      isDefaultFetchPort = port == "443"_kj || port == "80"_kj;
      domain = kj::str(host);
    }
    KJ_CASE_ONEOF(record, SocketAddress) {
      domain = kj::heapString(record.hostname);
      isDefaultFetchPort = record.port == 443 || record.port == 80;
    }
  }

  // Convert the address to a string that we can pass to kj.
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

  jsg::Ref<Fetcher> actualFetcher = nullptr;
  KJ_IF_SOME(f, fetcher) {
    actualFetcher = kj::mv(f);
  } else {
    // Support calling into arbitrary callbacks for any registered "magic" addresses for which
    // custom connect() logic is needed. Note that these overrides should only apply to calls of the
    // global connect() method, not for fetcher->connect(), hence why we check for them here.
    KJ_IF_SOME(fn, ioContext.getCurrentLock().getWorker().getConnectOverride(addressStr)) {
      return fn(js);
    }
    actualFetcher =
        js.alloc<Fetcher>(IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
  }

  CfProperty cf;
  kj::Own<WorkerInterface> client =
      actualFetcher->getClient(ioContext, cf.serialize(js), "connect"_kjc);

  // Set up the connection.
  auto headers = kj::heap<kj::HttpHeaders>(ioContext.getHeaderTable());
  auto httpClient = asHttpClient(kj::mv(client));
  kj::HttpConnectSettings httpConnectSettings = {.useTls = false};
  SecureTransportKind secureTransport = SecureTransportKind::OFF;
  KJ_IF_SOME(opts, options) {
    secureTransport = parseSecureTransport(opts);
    httpConnectSettings.useTls = secureTransport == SecureTransportKind::ON;
  }
  kj::Own<kj::TlsStarterCallback> tlsStarter = kj::heap<kj::TlsStarterCallback>();
  httpConnectSettings.tlsStarter = tlsStarter;
  auto request = httpClient->connect(addressStr, *headers, httpConnectSettings);
  request.connection = request.connection.attach(kj::mv(httpClient));

  auto result = setupSocket(js, kj::mv(request.connection), kj::mv(addressStr), kj::mv(options),
      kj::mv(tlsStarter), secureTransport, kj::mv(domain), isDefaultFetchPort,
      kj::none /* maybeOpenedPrPair */);
  // `handleProxyStatus` needs an initialized refcount to use `JSG_THIS`, hence it cannot be
  // called in Socket's constructor. Also it's only necessary when creating a Socket as a result of
  // a `connect`.
  result->handleProxyStatus(js, kj::mv(request.status));
  return result;
}

jsg::Ref<Socket> connectImpl(jsg::Lock& js,
    kj::Maybe<jsg::Ref<Fetcher>> fetcher,
    AnySocketAddress address,
    jsg::Optional<SocketOptions> options) {
  // TODO(soon): Doesn't this need to check for the presence of an output lock, and if it finds one
  // then wait on it, before calling into connectImplNoOutputLock?
  return connectImplNoOutputLock(js, kj::mv(fetcher), kj::mv(address), kj::mv(options));
}

jsg::Promise<void> Socket::close(jsg::Lock& js) {
  if (isClosing) {
    return closedPromiseCopy.whenResolved(js);
  }

  isClosing = true;
  writable->getController().setPendingClosure();
  readable->getController().setPendingClosure();

  // Wait until the socket connects (successfully or otherwise)
  return openedPromiseCopy.whenResolved(js)
      .then(js,
          [this](jsg::Lock& js) {
    if (!writable->getController().isClosedOrClosing()) {
      return writable->getController().flush(js);
    } else {
      return js.resolvedPromise();
    }
  })
      .then(js,
          [this](jsg::Lock& js) {
    // Forcibly abort the readable/writable streams.
    auto cancelPromise = readable->getController().cancel(js, kj::none);
    auto abortPromise = writable->getController().abort(js, kj::none);
    // The below is effectively `Promise.all(cancelPromise, abortPromise)`
    return cancelPromise.then(js, [abortPromise = kj::mv(abortPromise)](jsg::Lock& js) mutable {
      return kj::mv(abortPromise);
    });
  })
      .then(js, [this](jsg::Lock& js) {
    resolveFulfiller(js, kj::none);
    return js.resolvedPromise();
  }).catch_(js, [this](jsg::Lock& js, jsg::Value err) { errorHandler(js, kj::mv(err)); });
}

jsg::Ref<Socket> Socket::startTls(jsg::Lock& js, jsg::Optional<TlsOptions> tlsOptions) {
  JSG_REQUIRE(
      secureTransport != SecureTransportKind::ON, TypeError, "Cannot startTls on a TLS socket.");
  // TODO: Track closed state of socket properly and assert that it hasn't been closed here.
  JSG_REQUIRE(domain != nullptr, TypeError, "startTls can only be called once.");
  auto invalidOptKindMsg =
      "The `secureTransport` socket option must be set to 'starttls' for startTls to be used.";
  JSG_REQUIRE(secureTransport == SecureTransportKind::STARTTLS, TypeError, invalidOptKindMsg);

  // The current socket's writable buffers need to be flushed. The socket's WritableStream is backed
  // by an AsyncIoStream which doesn't implement any buffering, so we don't need to worry about
  // flushing. But the JS WritableStream holds a queue so some data may still be buffered. This
  // means we need to flush the WritableStream.
  //
  // Detach the AsyncIoStream from the Writable/Readable streams and make them unusable.
  auto& context = IoContext::current();
  auto openedPrPair = js.newPromiseAndResolver<SocketInfo>();
  auto secureStreamPromise = context.awaitJs(js,
      writable->flush(js).then(js,
          // The openedResolver is a jsg::Promise::Resolver. It should be gc visited here in
          // case the opened promise is resolves captures a circular references to itself in
          // JavaScript (which is most likely). This prevents a possible memory leak.
          // We also capture a strong reference to the original Socket instance that is being
          // upgraded in order to prevent it from being GC'd while we are waiting for the
          // flush to complete. While it is unlikely to be GC'd while we are waiting because
          // the user code *likely* is holding a active reference to it at this point, we
          // don't want to take any chances. This prevents a possible UAF.
          JSG_VISITABLE_LAMBDA((self = JSG_THIS, domain = kj::heapString(domain),
                                   tlsOptions = kj::mv(tlsOptions), tlsStarter = kj::mv(tlsStarter),
                                   openedResolver = openedPrPair.resolver.addRef(js),
                                   remoteAddress = kj::str(remoteAddress)),
              (self, openedResolver), (jsg::Lock & js) mutable {
                auto& context = IoContext::current();

                self->writable->detach(js);
                self->readable = self->readable->detach(js, true);

                // We should set this before closedResolver.resolve() in order to give the user
                // the option to check if the closed promise is resolved due to upgrade or not.
                self->upgraded = true;
                self->closedResolver.resolve(js);

                auto acceptedHostname = domain.asPtr();
                KJ_IF_SOME(s, tlsOptions) {
                KJ_IF_SOME(expectedHost, s.expectedServerHostname) {
                acceptedHostname = expectedHost;
                } else {
                }  // Needed to avoid compiler error/warning
                } else {
                }  // Needed to avoid compiler error/warning

                // All non-secure sockets should have a tlsStarter. Though since tlsStarter is an
                // IoOwn, if the request's IoContext has ended then `tlsStarter` will be null. This
                // can happen if the flush operation is taking a particularly long time (EW-8538),
                // so we throw a JSG error if that's the case.
                JSG_REQUIRE(*tlsStarter != kj::none, TypeError,
                    "The request has finished before startTls completed.");

                // Fork the starter promise because we need to create two separate things waiting
                // on it below. The first is resolving the openedResolver with a JS promise that
                // wraps one branch, the secnod is the kj::Promise that we use to resolve the
                // secureStream for the promised stream. This keeps us from having to bounce in and
                // out of the JS isolate lock.
                auto forkedPromise = KJ_ASSERT_NONNULL(*tlsStarter)(acceptedHostname).fork();

                openedResolver.resolve(js,
                    context.awaitIo(js, forkedPromise.addBranch(),
                        [remoteAddress = kj::mv(remoteAddress)](
                            jsg::Lock& js) mutable -> SocketInfo {
                  return SocketInfo{
                    .remoteAddress = kj::mv(remoteAddress),
                    .localAddress = kj::none,
                  };
                }));

                auto secureStream = forkedPromise.addBranch().then(
                    [stream = self->connectionStream->addWrappedRef()]() mutable
                    -> kj::Own<kj::AsyncIoStream> { return kj::mv(stream); });

                return kj::newPromisedStream(kj::mv(secureStream));
              })));

  // The existing tlsStarter gets consumed and we won't need it again. Pass in an empty tlsStarter
  // to `setupSocket`.
  auto newTlsStarter = kj::heap<kj::TlsStarterCallback>();
  return setupSocket(js, kj::newPromisedStream(kj::mv(secureStreamPromise)), kj::str(remoteAddress),
      kj::mv(options), kj::mv(newTlsStarter), SecureTransportKind::ON, kj::mv(domain),
      isDefaultFetchPort, kj::mv(openedPrPair));
}

void Socket::handleProxyStatus(
    jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status) {
  auto& context = IoContext::current();
  auto errorHandler = [](kj::Exception&& e) {
    // Let's not log errors when we have a disconnected exception.
    // If we don't filter this out, whenever connect() fails, we'll
    // have noisy errors even though the user catches the error on JS side.
    if (e.getType() != kj::Exception::Type::DISCONNECTED) {
      LOG_ERROR_PERIODICALLY("Socket proxy disconnected abruptly", e);
    }
    return kj::HttpClient::ConnectRequest::Status(500, nullptr, kj::Own<kj::HttpHeaders>());
  };
  auto func = [this, self = JSG_THIS](
                  jsg::Lock& js, kj::HttpClient::ConnectRequest::Status&& status) -> void {
    if (status.statusCode < 200 || status.statusCode >= 300) {
      // If the status indicates an unsuccessful connection we need to reject the `closeFulfiller`
      // with an exception. This will reject the socket's `closed` promise.
      auto msg = kj::str("proxy request failed, cannot connect to the specified address");
      if (isDefaultFetchPort) {
        msg = kj::str(msg, ". It looks like you might be trying to connect to a HTTP-based service",
            " — consider using fetch instead");
      }
      handleProxyError(js, JSG_KJ_EXCEPTION(FAILED, Error, msg));
    } else {
      // In our implementation we do not expose the local address at all simply
      // because there's no useful value we can provide.
      openedResolver.resolve(js,
          SocketInfo{
            .remoteAddress = kj::str(remoteAddress),
            .localAddress = kj::none,
          });
    }
  };
  auto result = context.awaitIo(js, status.catch_(kj::mv(errorHandler)), kj::mv(func));
  result.markAsHandled(js);
}

void Socket::handleProxyStatus(jsg::Lock& js, kj::Promise<kj::Maybe<kj::Exception>> connectResult) {
  // It's kind of weird to take a promise that resolves to a Maybe<Exception> but we can't just use
  // a Promise<void> and put our logic in the error handler because awaitIo doesn't provide the
  // jsg::Lock for void promises or to errorFunc implementations, only non-void success callbacks,
  // but we need the lock in our callback here.
  // TODO(cleanup): Extend awaitIo to provide the jsg::Lock in more cases.
  auto& context = IoContext::current();
  auto errorHandler = [](kj::Exception&& e) -> kj::Maybe<kj::Exception> {
    LOG_ERROR_PERIODICALLY("Socket proxy disconnected abruptly", e);
    return KJ_EXCEPTION(FAILED, "connectResult raised an error");
  };
  auto func = [this, self = JSG_THIS](jsg::Lock& js, kj::Maybe<kj::Exception> result) -> void {
    if (result != kj::none) {
      handleProxyError(js, JSG_KJ_EXCEPTION(FAILED, Error, "connection attempt failed"));
    } else {
      // In our implementation we do not expose the local address at all simply
      // because there's no useful value we can provide.
      openedResolver.resolve(js,
          SocketInfo{
            .remoteAddress = kj::str(remoteAddress),
            .localAddress = kj::none,
          });
    }
  };
  auto result = context.awaitIo(js, connectResult.catch_(kj::mv(errorHandler)), kj::mv(func));
  result.markAsHandled(js);
}

void Socket::handleProxyError(jsg::Lock& js, kj::Exception e) {
  resolveFulfiller(js, kj::cp(e));
  openedResolver.reject(js, kj::cp(e));
  readable->getController().cancel(js, kj::none).markAsHandled(js);
  writable->getController().abort(js, js.error(e.getDescription())).markAsHandled(js);
}

void Socket::handleReadableEof(jsg::Lock& js, jsg::Promise<void> onEof) {
  KJ_ASSERT(!getAllowHalfOpen(options));
  // Listen for EOF on the ReadableStream.
  onEof
      .then(
          js,
          JSG_VISITABLE_LAMBDA(
              (ref = JSG_THIS), (ref), (jsg::Lock& js) { return ref->maybeCloseWriteSide(js); }))
      .markAsHandled(js);
}

jsg::Promise<void> Socket::maybeCloseWriteSide(jsg::Lock& js) {
  // When `allowHalfOpen` is set to true then we do not automatically close the write side on EOF.
  // This code shouldn't even run since we don't set up a callback which calls it unless
  // `allowHalfOpen` is false.
  KJ_ASSERT(!getAllowHalfOpen(options));

  // Do not call `close` on a controller that has already been closed or is in the process
  // of closing.
  if (writable->getController().isClosedOrClosing()) {
    return js.resolvedPromise();
  }

  // We want to close the socket, but only after its WritableStream has been flushed. We do this
  // below by calling `close` on the WritableStream which ensures that any data pending on it
  // is flushed. Then once the `close` either completes or fails we can be sure that any data has
  // been flushed.
  return writable->getController()
      .close(js)
      .catch_(js,
          JSG_VISITABLE_LAMBDA((ref = JSG_THIS), (ref),
              (jsg::Lock& js, jsg::Value&& exc) {
                ref->closedResolver.reject(js, exc.getHandle(js));
              }))
      .then(js, JSG_VISITABLE_LAMBDA((ref = JSG_THIS), (ref), (jsg::Lock& js) {
        ref->closedResolver.resolve(js);
      }));
}

jsg::Ref<Socket> SocketsModule::connect(
    jsg::Lock& js, AnySocketAddress address, jsg::Optional<SocketOptions> options) {
  return connectImpl(js, kj::none, kj::mv(address), kj::mv(options));
}

kj::Own<kj::AsyncIoStream> Socket::takeConnectionStream(jsg::Lock& js) {
  // We do not care if the socket was disturbed, we require the user to ensure the socket is not
  // being used.
  writable->detach(js);
  readable->detach(js, true);

  closedResolver.resolve(js);
  return connectionStream->addWrappedRef();
}

// Implementation of the custom factory for creating WorkerInterface instances from a socket
class StreamOutgoingFactory final: public Fetcher::OutgoingFactory, public kj::Refcounted {
 public:
  StreamOutgoingFactory(kj::Own<kj::AsyncIoStream> stream,
      kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable)
      : stream(kj::mv(stream)),
        httpClient(
            kj::newHttpClient(headerTable, *this->stream, {.entropySource = entropySource})) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override;

 private:
  kj::Own<kj::AsyncIoStream> stream;
  kj::Own<kj::HttpClient> httpClient;
  friend class StreamWorkerInterface;
};

// Definition of the StreamWorkerInterface class
class StreamWorkerInterface final: public WorkerInterface {
 public:
  StreamWorkerInterface(kj::Own<StreamOutgoingFactory> factory): factory(kj::mv(factory)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    // Parse the URL to extract the path
    auto parsedUrl = KJ_REQUIRE_NONNULL(kj::Url::tryParse(url, kj::Url::Context::HTTP_PROXY_REQUEST,
                                            {.percentDecode = false, .allowEmpty = true}),
        "invalid url", url);

    // We need to convert the URL from proxy format (full URL in request line) to host format
    // (path in request line, hostname in Host header).
    auto newHeaders = headers.cloneShallow();
    newHeaders.setPtr(kj::HttpHeaderId::HOST, parsedUrl.host);
    auto noHostUrl = parsedUrl.toString(kj::Url::Context::HTTP_REQUEST);

    // Create a new HTTP service from the client
    auto service = kj::newHttpService(*factory->httpClient);

    // Forward the request to the service
    co_await service->request(method, noHostUrl, newHeaders, requestBody, response);
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    JSG_FAIL_REQUIRE(TypeError,
        "connect is not something that can be done on a fetcher converted from a socket");
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED("prewarm() not supported on StreamWorkerInterface");
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED("runScheduled() not supported on StreamWorkerInterface");
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED("runAlarm() not supported on StreamWorkerInterface");
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

 private:
  kj::Own<StreamOutgoingFactory> factory;
};

kj::Own<WorkerInterface> StreamOutgoingFactory::newSingleUseClient(kj::Maybe<kj::String> cfStr) {
  JSG_ASSERT(stream.get() != nullptr, Error,
      "Fetcher created from internalNewHttpClient can only be used once");
  // Create a WorkerInterface that wraps the stream
  return kj::heap<StreamWorkerInterface>(kj::addRef(*this));
}

jsg::Promise<jsg::Ref<Fetcher>> SocketsModule::internalNewHttpClient(
    jsg::Lock& js, jsg::Ref<Socket> socket) {

  // TODO(soon) check for nothing to read, this will require things using a promise so this function
  // must remain returning a jsg::Promise waiting on a TODO for releaseLock

  // Flush the writable stream before taking the connection stream to ensure all data is written
  // before the stream is detatched
  return socket->getWritable()->flush(js).then(
      js, JSG_VISITABLE_LAMBDA((socket = kj::mv(socket)), (socket), (jsg::Lock & js) mutable {
        auto& ioctx = IoContext::current();

        // Create our custom factory that will create client instances from this socket
        kj::Own<Fetcher::OutgoingFactory> outgoingFactory = kj::refcounted<StreamOutgoingFactory>(
            socket->takeConnectionStream(js), ioctx.getEntropySource(), ioctx.getHeaderTable());

        // Create a Fetcher that uses our custom factory
        auto fetcher = js.alloc<Fetcher>(
            ioctx.addObject(kj::mv(outgoingFactory)), Fetcher::RequiresHostAndProtocol::YES);

        return kj::mv(fetcher);
      }));
}
}  // namespace workerd::api
