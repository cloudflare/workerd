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
    std::function<void()> onClose) {
  auto& context = IoContext::current();

  // Initialise the readable/writable streams with a custom AsyncIoStream that waits for the
  // completion of `connectionPromise` before performing reads/writes.
  auto stream = kj::refcounted<PipelinedAsyncIoStream>(kj::mv(connectionPromise), onClose);
  auto sysStreams = newSystemMultiStream(kj::addRef(*stream), StreamEncoding::IDENTITY, context);
  auto readable = jsg::alloc<ReadableStream>(context, kj::mv(sysStreams.readable));
  auto writable = jsg::alloc<WritableStream>(context, kj::mv(sysStreams.writable));

  auto closeFulfiller = IoContext::current().addObject(
      kj::heap<kj::PromiseFulfillerPair<bool>>(kj::newPromiseAndFulfiller<bool>()));
  auto jsCloseFulfiller = IoContext::current().addObject(
      kj::heap<kj::PromiseFulfillerPair<void>>(kj::newPromiseAndFulfiller<void>()));
  auto jsCloseFulfillerFork = jsCloseFulfiller->promise.fork();

  return {
    .readable = kj::mv(readable),
    .writable = kj::mv(writable),
    .closeFulfiller = kj::mv(closeFulfiller),
    .jsCloseFulfiller = kj::mv(jsCloseFulfiller),
    .jsCloseFulfillerFork = kj::mv(jsCloseFulfillerFork)
  };
}

Socket::Socket(jsg::Lock& js, kj::Promise<kj::Own<kj::AsyncIoStream>> connectionPromise) :
    Socket(js, initialiseSocket(js, kj::mv(connectionPromise), [this]() { performClose(true); })) {};

void Socket::close() {
  performClose(false);
}

void Socket::performClose(bool isImplicit) {
  if (!closeFulfiller->fulfiller->isWaiting()) {
    return;
  }

  // Readable and writable streams are closed inside callback set up in `initialiseSocket`.
  closeFulfiller->fulfiller->fulfill(kj::mv(isImplicit));
}

PipelinedAsyncIoStream::PipelinedAsyncIoStream(kj::Promise<kj::Own<kj::AsyncIoStream>> inner,
    std::function<void()> onClose) :
    inner(kj::mv(inner)), onClose(onClose) {};

void PipelinedAsyncIoStream::shutdownWrite() {
  thenOrRunNow<void>([this](kj::AsyncIoStream& stream) {
    stream.shutdownWrite();
    onClose();
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::abortRead() {
  thenOrRunNow<void>([this](kj::AsyncIoStream& stream) {
    stream.abortRead();
    onClose();
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::getsockopt(int level, int option, void* value, uint* length) {
  thenOrRunNow<void>([=](kj::AsyncIoStream& stream) {
    stream.getsockopt(level, option, value, length);
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::setsockopt(int level, int option, const void* value, uint length) {
  thenOrRunNow<void>([=](kj::AsyncIoStream& stream) {
    stream.setsockopt(level, option, value, length);
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::getsockname(struct sockaddr* addr, uint* length) {
  thenOrRunNow<void>([=](kj::AsyncIoStream& stream) {
    stream.getsockname(addr, length);
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::getpeername(struct sockaddr* addr, uint* length) {
  thenOrRunNow<void>([=](kj::AsyncIoStream& stream) {
    stream.getpeername(addr, length);
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

kj::Promise<size_t> PipelinedAsyncIoStream::read(void* buffer, size_t minBytes, size_t maxBytes) {
  return thenOrRunNow<size_t>([=](kj::AsyncIoStream& stream) {
    auto& context = IoContext::current();
    return context.awaitJs(context.awaitIo(
        stream.read(buffer, minBytes, maxBytes), [this](size_t size) {
      if (size == 0) {
        onClose();
      }
      return size;
    }));
  });
}

kj::Promise<size_t> PipelinedAsyncIoStream::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  return thenOrRunNow<size_t>([=](kj::AsyncIoStream& stream) -> kj::Promise<size_t> {
    auto& context = IoContext::current();
    return context.awaitJs(context.awaitIo(
        stream.tryRead(buffer, minBytes, maxBytes), [this](size_t size) {
      if (size == 0) {
        onClose();
      }
      return size;
    }));
  });
}

kj::Promise<void> PipelinedAsyncIoStream::write(const void* buffer, size_t size) {
  return thenOrRunNow<void>([=](kj::AsyncIoStream& stream) {
    return stream.write(buffer, size);
  });
}

kj::Promise<void> PipelinedAsyncIoStream::write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  return thenOrRunNow<void>([=](kj::AsyncIoStream& stream) {
    return stream.write(pieces);
  });
}

kj::Promise<void> PipelinedAsyncIoStream::whenWriteDisconnected() {
  return thenOrRunNow<void>([=](kj::AsyncIoStream& stream) {
    auto& context = IoContext::current();
    return context.awaitJs(context.awaitIo(
        stream.whenWriteDisconnected(), [this]() {
      onClose();
    }));
  });
}

}  // namespace workerd::api
