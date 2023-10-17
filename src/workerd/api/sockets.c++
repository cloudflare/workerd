// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sockets.h"
#include "system-streams.h"
#include <workerd/io/worker-interface.h>
#include "url-standard.h"


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

  for (auto i : kj::indices(host)) {
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

SecureTransportKind parseSecureTransport(SocketOptions& opts) {
  auto value = KJ_UNWRAP_OR_RETURN(opts.secureTransport, SecureTransportKind::OFF).begin();
  if (value == "off"_kj) {
    return SecureTransportKind::OFF;
  } else if (value == "starttls"_kj) {
    return SecureTransportKind::STARTTLS;
  } else if (value == "on"_kj) {
    return SecureTransportKind::ON;
  } else {
    JSG_FAIL_REQUIRE(TypeError,
        kj::str("Unsupported value in secureTransport socket option: ", value));
  }
}

bool getAllowHalfOpen(jsg::Optional<SocketOptions>& opts) {
  KJ_IF_SOME(o, opts) {
    return o.allowHalfOpen;
  }

  // The allowHalfOpen flag is false by default.
  return false;
}

} // namespace

jsg::Ref<Socket> setupSocket(
    jsg::Lock& js, kj::Own<kj::AsyncIoStream> connection,
    jsg::Optional<SocketOptions> options, kj::Own<kj::TlsStarterCallback> tlsStarter,
    bool isSecureSocket, kj::String domain, bool isDefaultFetchPort) {
  auto& ioContext = IoContext::current();

  // Disconnection handling is annoyingly complicated:
  //
  // We can't just context.awaitIo(connection->whenWriteDisconnected()) directly, because the
  // Socket could be GC'd before `whenWriteDisconnected()` completes, causing the underlying
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
  auto deferredCancelDisconnected = kj::defer(
      [fulfiller=kj::mv(disconnectedPaf.fulfiller)]() mutable {
    // In case the `whenWriteDisconected()` listener task is canceled without fulfilling the
    // fulfiller, we want to silently fulfill it. This will happen when the Socket is GC'd.
    fulfiller->fulfill(true);
  });

  static auto constexpr handleDisconnected =
      [](kj::AsyncIoStream& connection, kj::PromiseFulfiller<bool>& fulfiller)
          -> kj::Promise<void> {
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

  ioContext.awaitIo(js, kj::mv(disconnectedPaf.promise),
      [resolver = closedPrPair.resolver.addRef(js)](jsg::Lock& js, bool canceled) mutable {
    // We want to silently ignore the canceled case, without ever resolving anything. Note that
    // if the application actually fetches the `closed` promise, then the JSG glue will prevent
    // the socket from being GC'd until that promise resolves, so it won't be canceled.
    if (!canceled) {
      resolver.resolve(js);
    }
  }, [resolver = closedPrPair.resolver.addRef(js)](jsg::Lock& js, jsg::Value exception) mutable {
    resolver.reject(js, exception.getHandle(js));
  });

  auto refcountedConnection = kj::refcountedWrapper(kj::mv(connection));
  // Initialise the readable/writable streams with the readable/writable sides of an AsyncIoStream.
  auto sysStreams = newSystemMultiStream(refcountedConnection->addWrappedRef(), ioContext);
  auto readable = jsg::alloc<ReadableStream>(ioContext, kj::mv(sysStreams.readable));
  auto allowHalfOpen = getAllowHalfOpen(options);
  kj::Maybe<jsg::Promise<void>> eofPromise;
  if (!allowHalfOpen) {
    eofPromise = readable->onEof(js);
  }
  auto openedPrPair = js.newPromiseAndResolver<void>();
  openedPrPair.promise.markAsHandled(js);
  auto writable = jsg::alloc<WritableStream>(
      ioContext, kj::mv(sysStreams.writable), kj::none, openedPrPair.promise.whenResolved(js));

  auto result = jsg::alloc<Socket>(
      js, ioContext,
      kj::mv(refcountedConnection),
      kj::mv(readable),
      kj::mv(writable),
      kj::mv(closedPrPair),
      kj::mv(watchForDisconnectTask),
      kj::mv(options),
      kj::mv(tlsStarter),
      isSecureSocket,
      kj::mv(domain),
      isDefaultFetchPort,
      kj::mv(openedPrPair));

  KJ_IF_SOME(p, eofPromise) {
    result->handleReadableEof(js, kj::mv(p));
  }
  return result;
}

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options) {

  // Extract the domain/ip we are connecting to from the address.
  kj::String domain;
  bool isDefaultFetchPort = false;
  KJ_SWITCH_ONEOF(address) {
    KJ_CASE_ONEOF(str, kj::String) {
      // We need just the hostname part of the address, i.e. we want to strip out the port.
      // We do this using the standard URL parser since it will handle IPv6 for us as well.
      auto record = JSG_REQUIRE_NONNULL(url::URL::parse(jsg::usv(kj::str("https://", str))),
          TypeError, "Specified address could not be parsed.");
      auto& host = JSG_REQUIRE_NONNULL(record.host, TypeError,
          "Specified address is missing hostname.");
      // Note that there is an edge case here where the address containing `:443` will nonetheless
      // parse to a record whose `port` is set to nullptr.
      auto port = record.port.orDefault(443);
      isDefaultFetchPort = port == 443 || port == 80;
      domain = host.toStr();
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

  auto& ioContext = IoContext::current();

  JSG_REQUIRE(!ioContext.isFiddle(), TypeError, "Socket API not supported in web preview mode.");

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
    actualFetcher = jsg::alloc<Fetcher>(
        IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
  }

  auto jsRequest = Request::constructor(js, kj::str(addressStr), kj::none);
  kj::Own<WorkerInterface> client = actualFetcher->getClient(
      ioContext, jsRequest->serializeCfBlobJson(js), "connect"_kjc);

  // Set up the connection.
  auto headers = kj::heap<kj::HttpHeaders>(ioContext.getHeaderTable());
  auto httpClient = asHttpClient(kj::mv(client));
  kj::HttpConnectSettings httpConnectSettings = { .useTls = false };
  KJ_IF_SOME(opts, options) {
    httpConnectSettings.useTls =
        parseSecureTransport(opts) == SecureTransportKind::ON;
  }
  kj::Own<kj::TlsStarterCallback> tlsStarter = kj::heap<kj::TlsStarterCallback>();
  httpConnectSettings.tlsStarter = tlsStarter;
  auto request = httpClient->connect(addressStr, *headers, httpConnectSettings);
  request.connection = request.connection.attach(kj::mv(httpClient));

  auto result = setupSocket(
      js, kj::mv(request.connection), kj::mv(options), kj::mv(tlsStarter),
      httpConnectSettings.useTls, kj::mv(domain), isDefaultFetchPort);
  // `handleProxyStatus` needs an initialised refcount to use `JSG_THIS`, hence it cannot be
  // called in Socket's constructor. Also it's only necessary when creating a Socket as a result of
  // a `connect`.
  result->handleProxyStatus(js, kj::mv(request.status));
  return result;
}

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options) {
  // TODO(soon): Doesn't this need to check for the presence of an output lock, and if it finds one
  // then wait on it, before calling into connectImplNoOutputLock?
  return connectImplNoOutputLock(js, kj::mv(fetcher), kj::mv(address), kj::mv(options));
}

jsg::Promise<void> Socket::close(jsg::Lock& js) {
  // Forcibly close the readable/writable streams.
  auto cancelPromise = readable->getController().cancel(js, kj::none);
  auto abortPromise = writable->getController().abort(js, kj::none);
  // The below is effectively `Promise.all(cancelPromise, abortPromise)`
  return cancelPromise.then(js, [abortPromise = kj::mv(abortPromise), this](jsg::Lock& js) mutable {
    return abortPromise.then(js, [this](jsg::Lock& js) {
      resolveFulfiller(js, kj::none);
      return js.resolvedPromise();
    }, [this](jsg::Lock& js, jsg::Value err) { return errorHandler(js, kj::mv(err)); });
  }, [this](jsg::Lock& js, jsg::Value err) { return errorHandler(js, kj::mv(err)); });
}

jsg::Ref<Socket> Socket::startTls(jsg::Lock& js, jsg::Optional<TlsOptions> tlsOptions) {
  JSG_REQUIRE(!isSecureSocket, TypeError, "Cannot startTls on a TLS socket.");
  // TODO: Track closed state of socket properly and assert that it hasn't been closed here.
  JSG_REQUIRE(domain != nullptr, TypeError, "startTls can only be called once.");
  auto invalidOptKindMsg =
      "The `secureTransport` socket option must be set to 'starttls' for startTls to be used.";
  KJ_IF_SOME(opts, options) {
    JSG_REQUIRE(parseSecureTransport(opts) == SecureTransportKind::STARTTLS,
        TypeError, invalidOptKindMsg);
  } else {
    JSG_FAIL_REQUIRE(TypeError, invalidOptKindMsg);
  }

  // The current socket's writable buffers need to be flushed. The socket's WritableStream is backed
  // by an AsyncIoStream which doesn't implement any buffering, so we don't need to worry about
  // flushing. But the JS WritableStream holds a queue so some data may still be buffered. This
  // means we need to flush the WritableStream.
  //
  // Detach the AsyncIoStream from the Writable/Readable streams and make them unusable.
  auto& context = IoContext::current();
  auto secureStreamPromise = context.awaitJs(js, writable->flush(js).then(js,
      [this, domain = kj::heapString(domain), tlsOptions = kj::mv(tlsOptions),
      tlsStarter = kj::mv(tlsStarter)](jsg::Lock& js) mutable {
    writable->removeSink(js);
    readable = readable->detach(js, true);
    closedResolver.resolve(js);

    auto acceptedHostname = domain.asPtr();
    KJ_IF_SOME(s, tlsOptions) {
      KJ_IF_SOME(expectedHost, s.expectedServerHostname) {
        acceptedHostname = expectedHost;
      }
    }

    // All non-secure sockets should have a tlsStarter.
    auto secureStream = KJ_ASSERT_NONNULL(*tlsStarter)(acceptedHostname).then(
      [stream = connectionStream->addWrappedRef()]() mutable -> kj::Own<kj::AsyncIoStream> {
        return kj::mv(stream);
      });
    return kj::newPromisedStream(kj::mv(secureStream));
  }));

  // The existing tlsStarter gets consumed and we won't need it again. Pass in an empty tlsStarter
  // to `setupSocket`.
  auto newTlsStarter = kj::heap<kj::TlsStarterCallback>();
  return setupSocket(js, kj::newPromisedStream(kj::mv(secureStreamPromise)), kj::mv(options),
      kj::mv(newTlsStarter), true, kj::mv(domain), isDefaultFetchPort);
}

void Socket::handleProxyStatus(
    jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status) {
  auto& context = IoContext::current();
  auto result = context.awaitIo(js,
      status.catch_([](kj::Exception&& e) {
        LOG_ERROR_PERIODICALLY("Socket proxy disconnected abruptly", e);
        return kj::HttpClient::ConnectRequest::Status(500, nullptr, kj::Own<kj::HttpHeaders>());
      }),
      [this, self = JSG_THIS](jsg::Lock& js, kj::HttpClient::ConnectRequest::Status&& status) -> void {
    if (status.statusCode < 200 || status.statusCode >= 300) {
      // If the status indicates an unsucessful connection we need to reject the `closeFulfiller`
      // with an exception. This will reject the socket's `closed` promise.
      auto msg = kj::str("proxy request failed, cannot connect to the specified address");
      if (isDefaultFetchPort) {
        msg = kj::str(msg, ". It looks like you might be trying to connect to a HTTP-based service",
            " — consider using fetch instead");
      }
      handleProxyError(js, JSG_KJ_EXCEPTION(FAILED, Error, msg));
    } else {
      openedResolver.resolve(js);
    }
  });
  result.markAsHandled(js);
}

void Socket::handleProxyStatus(jsg::Lock& js, kj::Promise<kj::Maybe<kj::Exception>> connectResult) {
  // It's kind of weird to take a promise that resolves to a Maybe<Exception> but we can't just use
  // a Promise<void> and put our logic in the error handler because awaitIo doesn't provide the
  // jsg::Lock for void promises or to errorFunc implementations, only non-void success callbacks,
  // but we need the lock in our callback here.
  // TODO(cleanup): Extend awaitIo to provide the jsg::Lock in more cases.
  auto& context = IoContext::current();
  auto result = context.awaitIo(js,
      connectResult.catch_([](kj::Exception&& e) -> kj::Maybe<kj::Exception> {
        LOG_ERROR_PERIODICALLY("Socket proxy disconnected abruptly", e);
        return KJ_EXCEPTION(FAILED, "connectResult raised an error");
      }),
      [this, self = JSG_THIS](jsg::Lock& js, kj::Maybe<kj::Exception> result) -> void {
    if (result != kj::none) {
      handleProxyError(js, JSG_KJ_EXCEPTION(FAILED, Error, "connection attempt failed"));
    } else {
      openedResolver.resolve(js);
    }
  });
  result.markAsHandled(js);
}

void Socket::handleProxyError(jsg::Lock& js, kj::Exception e) {
  resolveFulfiller(js, kj::cp(e));
  openedResolver.reject(js, kj::mv(e));
  readable->getController().cancel(js, kj::none).markAsHandled(js);
  writable->getController().abort(js, js.error(e.getDescription())).markAsHandled(js);
}

void Socket::handleReadableEof(jsg::Lock& js, jsg::Promise<void> onEof) {
  KJ_ASSERT(!getAllowHalfOpen(options));
  // Listen for EOF on the ReadableStream.
  onEof.then(js,
      JSG_VISITABLE_LAMBDA((ref=JSG_THIS), (ref), (jsg::Lock& js) {
    return ref->maybeCloseWriteSide(js);
  })).markAsHandled(js);
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
  return writable->getController().close(js).catch_(js,
      JSG_VISITABLE_LAMBDA((ref=JSG_THIS), (ref), (jsg::Lock& js, jsg::Value&& exc) {
    ref->closedResolver.reject(js, exc.getHandle(js));
  })).then(js, JSG_VISITABLE_LAMBDA((ref=JSG_THIS), (ref), (jsg::Lock& js) {
    ref->closedResolver.resolve(js);
  }));
}

}  // namespace workerd::api
