// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sockets.h"
#include "system-streams.h"


namespace workerd::api {


jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, kj::String address) {
  auto& ioContext = IoContext::current();

  auto jsRequest = Request::constructor(js, kj::str(address), nullptr);
  kj::Own<WorkerInterface> client = fetcher->getClient(
      ioContext, jsRequest->serializeCfBlobJson(js), "connect"_kj);

  // TODO: Validate `address` is well formed. Do I need to use `parseAddress` here or is there
  // a better way?

  // Set up the connection.
  auto headers = kj::heap<kj::HttpHeaders>(ioContext.getHeaderTable());
  auto httpClient = kj::newHttpClient(*client);
  auto request = httpClient->connect(address, *headers);
  // TODO(soon): If `request.status` resolves to have a statusCode < 200 || >= 300, arrange for the
  //   the Socket to throw an appropriate error. Right now in this circumstance,
  //   `request.connection`'s operations will throw KJ exceptions which will be exposed to the
  //   script as internal errors.
  return jsg::alloc<Socket>(js, request.connection.attach(kj::mv(request.status)));
}

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, kj::String address,
    CompatibilityFlags::Reader featureFlags) {
  if (!featureFlags.getTcpSocketsSupport()) {
    JSG_FAIL_REQUIRE(TypeError, "TCP Sockets API not enabled.");
  }
  jsg::Ref<Fetcher> actualFetcher = nullptr;
  KJ_IF_MAYBE(f, fetcher) {
    actualFetcher = kj::mv(*f);
  } else {
    actualFetcher = jsg::alloc<Fetcher>(
        IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
  }
  return connectImplNoOutputLock(js, kj::mv(actualFetcher), kj::mv(address));
}

InitData initialiseSocket(
    jsg::Lock& js, kj::Promise<kj::Own<kj::AsyncIoStream>> connectionPromise,
    kj::Function<void()> onCloseRead) {
  auto& context = IoContext::current();

  // Initialise the readable/writable streams with a promised AsyncIoStream that waits for the
  // completion of `connectionPromise` before performing reads/writes.
  auto stream = kj::heap<NotifiedAsyncIoStream>(
      kj::newPromisedStream(kj::mv(connectionPromise)), kj::mv(onCloseRead));
  auto sysStreams = newSystemMultiStream(kj::mv(stream), context);
  auto readable = jsg::alloc<ReadableStream>(context, kj::mv(sysStreams.readable));
  auto writable = jsg::alloc<WritableStream>(context, kj::mv(sysStreams.writable));

  auto closeFulfiller = IoContext::current().addObject(kj::heap<jsg::PromiseResolverPair<void>>(
      jsg::newPromiseAndResolver<void>(context.getCurrentLock().getIsolate())));

  return {
    .readable = kj::mv(readable),
    .writable = kj::mv(writable),
    .closeFulfiller = kj::mv(closeFulfiller)
  };
}

Socket::Socket(jsg::Lock& js, kj::Promise<kj::Own<kj::AsyncIoStream>> connectionPromise) :
    Socket(js, initialiseSocket(js, kj::mv(connectionPromise), [this]() { readSideClose(); })) {};

void Socket::close() {
  if (isClosed) {
    return;
  }

  // Forcibly close the readable/writable streams.
  auto& context = IoContext::current();
  context.addTask(context.run([this](jsg::Lock& js) {
    readable->getController().cancel(js, nullptr).then(js, [this](
        jsg::Lock& js) mutable {
      writable->getController().abort(js, nullptr).then(js,
          [this](jsg::Lock& js) mutable { resolveFulfiller(nullptr); },
          [this](jsg::Lock& js, jsg::Value err) { errorHandler(js, kj::mv(err)); });
    }, [this](jsg::Lock& js, jsg::Value err) { errorHandler(js, kj::mv(err)); });
  }));
}

void Socket::readSideClose() {
  // This is called when the read-side of the socket reads EOF (0 bytes). When that happens we
  // want any existing data on the WritableStream to be flushed. We can safely request to close the
  // WritableStream because any data pending on it will be flushed before it is closed.
  auto& context = IoContext::current();
  context.addTask(context.run([this](jsg::Lock& js) {
    return writable->getController().close(js).then(js, [this](jsg::Lock& js) {
      close();
    }, [this](jsg::Lock& js, jsg::Value err) {
      // The close can only fail if the WritableStream hasn't been attached, has already been
      // released or closed. We only need to ensure that the writable stream is flushed before
      // closing the socket, and since any errors here indicate that to be the case we can safely
      // close the socket.
      close();
    });
  }).ignoreResult());
}

NotifiedAsyncIoStream::NotifiedAsyncIoStream(kj::Own<kj::AsyncIoStream> inner,
    kj::Function<void()> onCloseRead) :
    inner(kj::mv(inner)), onCloseRead(kj::mv(onCloseRead)) {};

void NotifiedAsyncIoStream::shutdownWrite() {
  inner->shutdownWrite();
}

void NotifiedAsyncIoStream::abortRead() {
  inner->abortRead();
}

void NotifiedAsyncIoStream::getsockopt(int level, int option, void* value, uint* length) {
  inner->getsockopt(level, option, value, length);
}

void NotifiedAsyncIoStream::setsockopt(int level, int option, const void* value, uint length) {
  inner->setsockopt(level, option, value, length);
}

void NotifiedAsyncIoStream::getsockname(struct sockaddr* addr, uint* length) {
  inner->getsockname(addr, length);
}

void NotifiedAsyncIoStream::getpeername(struct sockaddr* addr, uint* length) {
  inner->getpeername(addr, length);
}

kj::Promise<size_t> NotifiedAsyncIoStream::read(void* buffer, size_t minBytes, size_t maxBytes) {
  auto& context = IoContext::current();
  return context.awaitJs(context.awaitIo(
      inner->read(buffer, minBytes, maxBytes), [this](size_t size) {
    if (size == 0) {
      onCloseRead();
    }
    return size;
  }));
}

kj::Promise<size_t> NotifiedAsyncIoStream::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  auto& context = IoContext::current();
  return context.awaitJs(context.awaitIo(
      inner->tryRead(buffer, minBytes, maxBytes), [this](size_t size) {
    if (size == 0) {
      onCloseRead();
    }
    return size;
  }));
}

kj::Promise<void> NotifiedAsyncIoStream::write(const void* buffer, size_t size) {
  return inner->write(buffer, size);
}

kj::Promise<void> NotifiedAsyncIoStream::write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  return inner->write(pieces);
}

kj::Promise<void> NotifiedAsyncIoStream::whenWriteDisconnected() {
  return inner->whenWriteDisconnected();
}

}  // namespace workerd::api
