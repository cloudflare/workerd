// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sockets.h"
#include "system-streams.h"


namespace workerd::api {


class TCPConnectResponseImpl final: public kj::HttpService::ConnectResponse, public kj::Refcounted {
public:
  TCPConnectResponseImpl(kj::Own<kj::PromiseFulfiller<kj::HttpClient::ConnectResponse>> fulfiller)
      : fulfiller(kj::mv(fulfiller)) {}

  void setPromise(kj::Promise<void> promise) {
    task = promise.eagerlyEvaluate([this](kj::Exception&& exception) {
      if (fulfiller->isWaiting()) {
        fulfiller->reject(kj::mv(exception));
      } else {
        kj::throwRecoverableException(kj::mv(exception));
      }
    });
  }

  kj::Own<kj::AsyncIoStream> accept(
      uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    KJ_REQUIRE(statusCode >= 200 && statusCode < 300, "the statusCode must be 2xx for accept");

    auto headersCopy = kj::heap(headers.clone());

    auto pipe = kj::newTwoWayPipe();

    fulfiller->fulfill(kj::HttpClient::ConnectResponse {
      statusCode,
      statusText,
      headersCopy.get(),
      pipe.ends[0].attach(kj::addRef(*this)),
    });
    return kj::mv(pipe.ends[1]);
  }

  kj::Own<kj::AsyncOutputStream> reject(
      uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    KJ_REQUIRE(statusCode < 200 || statusCode >= 300, "the statusCode must not be 2xx for reject.");
    kj::throwRecoverableException(KJ_EXCEPTION(FAILED,
        kj::str("jsg.Error: Connection rejected by proxy with HTTP code ", statusCode)));
    KJ_UNREACHABLE;
  }

private:
  kj::Own<kj::PromiseFulfiller<kj::HttpClient::ConnectResponse>> fulfiller;
  kj::Promise<void> task = nullptr;
};

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, kj::String address) {
  auto& ioContext = IoContext::current();

  auto jsRequest = Request::constructor(js, kj::str(address), nullptr);
  kj::Own<WorkerInterface> client = fetcher->getClient(
      ioContext, jsRequest->serializeCfBlobJson(js), "connect"_kj);

  // TODO: Validate `address` is well formed. Do I need to use `parseAddress` here or is there
  // a better way?

  // Set up the connection. This is similar to HttpClientAdapter::connect.
  auto headers = kj::heap<kj::HttpHeaders>(ioContext.getHeaderTable());
  auto paf = kj::newPromiseAndFulfiller<kj::HttpClient::ConnectResponse>();
  auto tunnel = kj::refcounted<TCPConnectResponseImpl>(kj::mv(paf.fulfiller));
  auto promise = client->connect(address, *headers, *tunnel)
      .attach(kj::str(address), kj::mv(headers));
  tunnel->setPromise(kj::mv(promise));
  kj::Promise<kj::HttpClient::ConnectResponse> connResponse = paf.promise.attach(kj::mv(tunnel));
  return jsg::alloc<Socket>(kj::mv(connResponse));
}

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, kj::String address) {
  jsg::Ref<Fetcher> actualFetcher = nullptr;
  KJ_IF_MAYBE(f, fetcher) {
    actualFetcher = kj::mv(*f);
  } else {
    actualFetcher = jsg::alloc<Fetcher>(
        IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
  }
  return connectImplNoOutputLock(js, kj::mv(actualFetcher), kj::mv(address));
}

kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection(
    kj::Promise<kj::HttpClient::ConnectResponse> connectionPromise) {
  return connectionPromise.then([](kj::HttpClient::ConnectResponse&& response)
      -> kj::Own<kj::AsyncIoStream> {
    KJ_REQUIRE(response.statusCode >= 200 && response.statusCode < 300,
        "the statusCode must be 2xx for connect");
    KJ_SWITCH_ONEOF(response.connectionOrBody) {
      KJ_CASE_ONEOF(connection, kj::Own<kj::AsyncIoStream>) {
        return kj::mv(connection);
      }
      KJ_CASE_ONEOF(body, kj::Own<kj::AsyncInputStream>) {
        kj::throwRecoverableException(
            KJ_EXCEPTION(FAILED, "jsg.Error: Could not establish proxy connection"));
      }
    }
    KJ_UNREACHABLE;
  });
}

InitData initialiseSocket(kj::Promise<kj::HttpClient::ConnectResponse> connectionPromise) {
  auto& context = IoContext::current();

  // Initialise the readable/writable streams with a custom AsyncIoStream that waits for the
  // completion of `connectionPromise` before performing reads/writes.
  auto stream = kj::refcounted<PipelinedAsyncIoStream>(processConnection(kj::mv(connectionPromise)));
  auto sysStreams = newSystemMultiStream(kj::addRef(*stream), StreamEncoding::IDENTITY, context);

  return {
    .readable = jsg::alloc<ReadableStream>(context, kj::mv(sysStreams.readable)),
    .writable = jsg::alloc<WritableStream>(context, kj::mv(sysStreams.writable)),
    .closeFulfiller = IoContext::current().addObject(
        kj::heap<kj::PromiseFulfillerPair<void>>(kj::newPromiseAndFulfiller<void>()))
  };
}

Socket::Socket(kj::Promise<kj::HttpClient::ConnectResponse> connectionPromise) :
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
