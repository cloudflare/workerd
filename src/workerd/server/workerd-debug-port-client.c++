// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workerd-debug-port-client.h"

#include <workerd/api/http.h>
#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>

namespace workerd::server {

namespace {
// A SubrequestChannel that wraps a WorkerdBootstrap capability.
// This allows us to make requests to a remote worker via the debug port.
class WorkerdBootstrapSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  WorkerdBootstrapSubrequestChannel(rpc::WorkerdBootstrap::Client bootstrap,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory)
      : bootstrap(kj::mv(bootstrap)),
        httpOverCapnpFactory(httpOverCapnpFactory),
        byteStreamFactory(byteStreamFactory) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    auto dispatcher = bootstrap.startEventRequest().send().getDispatcher();
    return kj::heap<RpcWorkerInterface>(
        httpOverCapnpFactory, byteStreamFactory, kj::mv(dispatcher));
  }

  void requireAllowsTransfer() override {
    JSG_FAIL_REQUIRE(Error, "WorkerdDebugPort bindings cannot be transferred to other workers");
  }

 private:
  rpc::WorkerdBootstrap::Client bootstrap;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  capnp::ByteStreamFactory& byteStreamFactory;
};

jsg::Ref<api::Fetcher> wrapBootstrapAsFetcher(
    jsg::Lock& js, IoContext& context, rpc::WorkerdBootstrap::Client bootstrap) {
  kj::Own<IoChannelFactory::SubrequestChannel> subrequestChannel =
      kj::refcounted<WorkerdBootstrapSubrequestChannel>(
          kj::mv(bootstrap), context.getHttpOverCapnpFactory(), context.getByteStreamFactory());
  return js.alloc<api::Fetcher>(
      context.addObject(kj::mv(subrequestChannel)), api::Fetcher::RequiresHostAndProtocol::NO);
}
}  // namespace

jsg::Promise<jsg::Ref<api::Fetcher>> WorkerdDebugPortClient::getEntrypoint(jsg::Lock& js,
    kj::String service,
    jsg::Optional<kj::String> entrypoint,
    jsg::Optional<jsg::JsRef<jsg::JsObject>> props) {
  auto& context = IoContext::current();

  auto req = debugPort.getEntrypointRequest();
  req.setService(service);
  KJ_IF_SOME(e, entrypoint) {
    req.setEntrypoint(e);
  }
  KJ_IF_SOME(p, props) {
    Frankenvalue::fromJs(js, p.getHandle(js)).toCapnp(req.initProps());
  }

  return context.awaitIo(
      js, req.send(), [&context](jsg::Lock& js, auto result) -> jsg::Ref<api::Fetcher> {
    return wrapBootstrapAsFetcher(js, context, result.getEntrypoint());
  });
}

jsg::Promise<jsg::Ref<api::Fetcher>> WorkerdDebugPortClient::getActor(
    jsg::Lock& js, kj::String service, kj::String entrypoint, kj::String actorId) {
  auto& context = IoContext::current();

  auto req = debugPort.getActorRequest();
  req.setService(service);
  req.setEntrypoint(entrypoint);
  req.setActorId(actorId);

  return context.awaitIo(
      js, req.send(), [&context](jsg::Lock& js, auto result) -> jsg::Ref<api::Fetcher> {
    return wrapBootstrapAsFetcher(js, context, result.getActor());
  });
}

jsg::Promise<jsg::Ref<WorkerdDebugPortClient>> WorkerdDebugPortConnector::connect(
    jsg::Lock& js, kj::String address) {
  auto& context = IoContext::current();
  auto connectPromise =
      context.getIoChannelFactory().getWorkerdDebugPortNetwork().parseAddress(address).then(
          [](kj::Own<kj::NetworkAddress> addr) { return addr->connect(); });

  return context.awaitIo(js, kj::mv(connectPromise),
      [](jsg::Lock& js, kj::Own<kj::AsyncIoStream> connection) -> jsg::Ref<WorkerdDebugPortClient> {
    auto rpcClient = kj::heap<capnp::TwoPartyClient>(*connection);
    auto debugPort = rpcClient->bootstrap().castAs<rpc::WorkerdDebugPort>();
    return js.alloc<WorkerdDebugPortClient>(
        kj::mv(connection), kj::mv(rpcClient), kj::mv(debugPort));
  });
}

}  // namespace workerd::server
