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

  // Note that we intentionally leave it up to the connect() implementation to decide what is a
  // valid address. This means that people using `workerd`, for example, can arrange to connect
  // to Unix sockets (if they define a "Network" service that permits local connections, which
  // the default internet service will not). Also, hypothetically, in W2W communications, the
  // address could be an arbitrary string which the receiving Worker can validate however it wants.
  //
  // TODO(soon): This results in an "internal error" in the case that the address couldn't parse,
  //   which is not a great experience. Should we attempt to validate the address here to give a
  //   better error? But that takes away the backend's flexibility to define its own address
  //   format. Maybe that's good though? It's more consistent with fetch(), which requires a
  //   valid URL even for W2W. The only other way we can get good errors here is if we use string
  //   matching to detect KJ's invalid-address error message, which seems pretty gross but could
  //   work. Note that if we do decide to validate addresses, we should not try to use KJ's
  //   `parseAddress()` but instead decide for ourselves what format we want to permit here, maybe
  //   as a regex.

  // Set up the connection.
  auto headers = kj::heap<kj::HttpHeaders>(ioContext.getHeaderTable());
  auto httpClient = kj::newHttpClient(*client);
  auto request = httpClient->connect(address, *headers);
  // TODO(soon): If `request.status` resolves to have a statusCode < 200 || >= 300, arrange for the
  //   the Socket to throw an appropriate error. Right now in this circumstance,
  //   `request.connection`'s operations will throw KJ exceptions which will be exposed to the
  //   script as internal errors.
  return jsg::alloc<Socket>(request.connection.attach(kj::mv(request.status)));
}

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, kj::String address,
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
  return connectImplNoOutputLock(js, kj::mv(actualFetcher), kj::mv(address));
}

InitData initialiseSocket(kj::Promise<kj::Own<kj::AsyncIoStream>> connectionPromise) {
  auto& context = IoContext::current();

  // Initialise the readable/writable streams with a custom AsyncIoStream that waits for the
  // completion of `connectionPromise` before performing reads/writes.
  auto stream = kj::refcounted<PipelinedAsyncIoStream>(kj::mv(connectionPromise));
  auto sysStreams = newSystemMultiStream(kj::addRef(*stream), StreamEncoding::IDENTITY, context);

  return {
    .readable = jsg::alloc<ReadableStream>(context, kj::mv(sysStreams.readable)),
    .writable = jsg::alloc<WritableStream>(context, kj::mv(sysStreams.writable)),
    .closeFulfiller = IoContext::current().addObject(
        kj::heap<kj::PromiseFulfillerPair<void>>(kj::newPromiseAndFulfiller<void>()))
  };
}

Socket::Socket(kj::Promise<kj::Own<kj::AsyncIoStream>> connectionPromise) :
    Socket(initialiseSocket(kj::mv(connectionPromise))) {};

jsg::Promise<void> Socket::close(jsg::Lock& js) {
  if (!closeFulfiller->fulfiller->isWaiting()) {
    return js.resolvedPromise();
  }

  auto result = js.resolvedPromise();
  result = readable->cancel(js, nullptr);
  result = writable->abort(js, nullptr);
  closeFulfiller->fulfiller->fulfill();
  return result;
}

PipelinedAsyncIoStream::PipelinedAsyncIoStream(kj::Promise<kj::Own<kj::AsyncIoStream>> inner) :
    inner(kj::mv(inner)) {};

void PipelinedAsyncIoStream::shutdownWrite() {
  thenOrRunNow<void>([](kj::Own<kj::AsyncIoStream>* stream) {
    (*stream)->shutdownWrite();
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::abortRead() {
  thenOrRunNow<void>([](kj::Own<kj::AsyncIoStream>* stream) {
    (*stream)->abortRead();
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::getsockopt(int level, int option, void* value, uint* length) {
  thenOrRunNow<void>([=](kj::Own<kj::AsyncIoStream>* stream) {
    (*stream)->getsockopt(level, option, value, length);
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::setsockopt(int level, int option, const void* value, uint length) {
  thenOrRunNow<void>([=](kj::Own<kj::AsyncIoStream>* stream) {
    (*stream)->setsockopt(level, option, value, length);
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::getsockname(struct sockaddr* addr, uint* length) {
  thenOrRunNow<void>([=](kj::Own<kj::AsyncIoStream>* stream) {
    (*stream)->getsockname(addr, length);
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

void PipelinedAsyncIoStream::getpeername(struct sockaddr* addr, uint* length) {
  thenOrRunNow<void>([=](kj::Own<kj::AsyncIoStream>* stream) {
    (*stream)->getpeername(addr, length);
    return kj::READY_NOW;
  }).detach([this](kj::Exception&& exception) mutable {
    error = kj::mv(exception);
  });
}

kj::Promise<size_t> PipelinedAsyncIoStream::read(void* buffer, size_t minBytes, size_t maxBytes) {
  return thenOrRunNow<size_t>([=](kj::Own<kj::AsyncIoStream>* stream) {
    return (*stream)->read(buffer, minBytes, maxBytes);
  });
}

kj::Promise<size_t> PipelinedAsyncIoStream::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  return thenOrRunNow<size_t>([=](kj::Own<kj::AsyncIoStream>* stream) {
    return (*stream)->tryRead(buffer, minBytes, maxBytes);
  });
}

kj::Promise<void> PipelinedAsyncIoStream::write(const void* buffer, size_t size) {
  return thenOrRunNow<void>([=](kj::Own<kj::AsyncIoStream>* stream) {
    return (*stream)->write(buffer, size);
  });
}

kj::Promise<void> PipelinedAsyncIoStream::write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  return thenOrRunNow<void>([=](kj::Own<kj::AsyncIoStream>* stream) {
    return (*stream)->write(pieces);
  });
}

kj::Promise<void> PipelinedAsyncIoStream::whenWriteDisconnected() {
  return thenOrRunNow<void>([=](kj::Own<kj::AsyncIoStream>* stream) {
    return (*stream)->whenWriteDisconnected();
  });
}

}  // namespace workerd::api
