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

  // Initialise the readable/writable streams with the readable/writable sides of an AsyncIoStream.
  auto sysStreams = newSystemMultiStream(kj::mv(request.connection), ioContext);
  auto readable = jsg::alloc<ReadableStream>(ioContext, kj::mv(sysStreams.readable));
  auto writable = jsg::alloc<WritableStream>(ioContext, kj::mv(sysStreams.writable));

  auto closeFulfiller = kj::heap<jsg::PromiseResolverPair<void>>(
      jsg::newPromiseAndResolver<void>(ioContext.getCurrentLock().getIsolate()));
  closeFulfiller->promise.markAsHandled();

  return jsg::alloc<Socket>(js, kj::mv(readable), kj::mv(writable), kj::mv(closeFulfiller));
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

}  // namespace workerd::api
