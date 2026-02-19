// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workerd-debug-port-client.h"

#include <workerd/api/http.h>
#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>

#include <kj/memory.h>

namespace workerd::server {

namespace {
// A SubrequestChannel that makes requests to a remote worker via the debug port.
//
// The connection ref is attached to WorkerInterfaces returned by startRequest().
// For HTTP fetch, the response body/WebSocket gets this attached (deferred proxying),
// ensuring the connection stays alive as long as the response is in use.
class WorkerdBootstrapSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  WorkerdBootstrapSubrequestChannel(rpc::WorkerdBootstrap::Client bootstrap,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::Own<DebugPortConnectionState> connectionState)
      : bootstrap(kj::mv(bootstrap)),
        httpOverCapnpFactory(httpOverCapnpFactory),
        byteStreamFactory(byteStreamFactory),
        connectionState(kj::mv(connectionState)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    // Pass cfBlobJson as an RPC parameter on startEvent so the server can include it
    // in SubrequestMetadata when creating the WorkerInterface.
    auto req = bootstrap.startEventRequest();
    KJ_IF_SOME(cf, metadata.cfBlobJson) {
      req.setCfBlobJson(cf);
    }
    auto dispatcher = req.send().getDispatcher();
    // Attach connection ref for deferred proxying - the HTTP response body/WebSocket
    // will get this WorkerInterface attached, keeping the connection alive.
    return kj::heap<RpcWorkerInterface>(httpOverCapnpFactory, byteStreamFactory, kj::mv(dispatcher))
        .attach(connectionState->addRef());
  }

  void requireAllowsTransfer() override {
    JSG_FAIL_REQUIRE(Error, "WorkerdDebugPort bindings cannot be transferred to other workers");
  }

 private:
  rpc::WorkerdBootstrap::Client bootstrap;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::Own<DebugPortConnectionState> connectionState;
};

jsg::Ref<api::Fetcher> wrapBootstrapAsFetcher(jsg::Lock& js,
    IoContext& context,
    rpc::WorkerdBootstrap::Client bootstrap,
    kj::Own<DebugPortConnectionState> connectionState) {
  kj::Own<IoChannelFactory::SubrequestChannel> subrequestChannel =
      kj::refcounted<WorkerdBootstrapSubrequestChannel>(kj::mv(bootstrap),
          context.getHttpOverCapnpFactory(), context.getByteStreamFactory(),
          kj::mv(connectionState));
  return js.alloc<api::Fetcher>(
      context.addObject(kj::mv(subrequestChannel)), api::Fetcher::RequiresHostAndProtocol::NO);
}
}  // namespace

jsg::Promise<jsg::Ref<api::Fetcher>> WorkerdDebugPortClient::getEntrypoint(jsg::Lock& js,
    kj::String service,
    jsg::Optional<kj::String> entrypoint,
    jsg::Optional<jsg::JsRef<jsg::JsObject>> props) {
  auto& context = IoContext::current();

  auto req = state->debugPort.getEntrypointRequest();
  req.setService(service);
  KJ_IF_SOME(e, entrypoint) {
    req.setEntrypoint(e);
  }
  KJ_IF_SOME(p, props) {
    Frankenvalue::fromJs(js, p.getHandle(js)).toCapnp(req.initProps());
  }

  return context.awaitIo(js, req.send(),
      [&context, stateRef = state->addRef()](
          jsg::Lock& js, auto result) mutable -> jsg::Ref<api::Fetcher> {
    return wrapBootstrapAsFetcher(js, context, result.getEntrypoint(), kj::mv(stateRef));
  });
}

jsg::Promise<jsg::Ref<api::Fetcher>> WorkerdDebugPortClient::getActor(
    jsg::Lock& js, kj::String service, kj::String entrypoint, kj::String actorId) {
  auto& context = IoContext::current();

  auto req = state->debugPort.getActorRequest();
  req.setService(service);
  req.setEntrypoint(entrypoint);
  req.setActorId(actorId);

  return context.awaitIo(js, req.send(),
      [&context, stateRef = state->addRef()](
          jsg::Lock& js, auto result) mutable -> jsg::Ref<api::Fetcher> {
    return wrapBootstrapAsFetcher(js, context, result.getActor(), kj::mv(stateRef));
  });
}

jsg::Promise<jsg::Ref<WorkerdDebugPortClient>> WorkerdDebugPortConnector::connect(
    jsg::Lock& js, kj::String address) {
  auto& context = IoContext::current();
  auto connectPromise =
      context.getIoChannelFactory().getWorkerdDebugPortNetwork().parseAddress(address).then(
          [](kj::Own<kj::NetworkAddress> addr) { return addr->connect(); });

  return context.awaitIo(js, kj::mv(connectPromise),
      [&context](jsg::Lock& js,
          kj::Own<kj::AsyncIoStream> connection) -> jsg::Ref<WorkerdDebugPortClient> {
    auto rpcClient = kj::heap<capnp::TwoPartyClient>(*connection);
    auto debugPort = rpcClient->bootstrap().castAs<rpc::WorkerdDebugPort>();
    auto state = kj::refcounted<DebugPortConnectionState>(
        kj::mv(connection), kj::mv(rpcClient), kj::mv(debugPort));
    return js.alloc<WorkerdDebugPortClient>(context.addObject(kj::mv(state)));
  });
}

}  // namespace workerd::server
