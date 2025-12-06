// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workerd-debug-port-client.h"

#include <workerd/api/http.h>
#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>

namespace workerd::server {

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
    // Use Cap'n Proto pipelining to get the dispatcher without waiting for the RPC
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

jsg::Promise<jsg::Ref<api::Fetcher>> WorkerdDebugPortClient::getEntrypoint(jsg::Lock& js,
    kj::String service,
    jsg::Optional<kj::String> entrypoint,
    jsg::Optional<jsg::JsRef<jsg::JsObject>> props) {
  auto& context = IoContext::current();

  // Prepare the props parameter before entering async code
  Frankenvalue frankenProps;
  KJ_IF_SOME(p, props) {
    frankenProps = Frankenvalue::fromJs(js, p.getHandle(js));
  }

  // Get the entrypoint name (may be none for default handler)
  kj::Maybe<kj::String> maybeEntrypoint;
  KJ_IF_SOME(e, entrypoint) {
    maybeEntrypoint = kj::mv(e);
  }

  // Get the debug port capability (async) and then send the request
  auto requestPromise = context.getIoChannelFactory().getWorkerdDebugPort(channel).then(
      [service = kj::mv(service), maybeEntrypoint = kj::mv(maybeEntrypoint),
          frankenProps = kj::mv(frankenProps)](capnp::Capability::Client cap) mutable {
    auto debugPort = cap.castAs<rpc::WorkerdDebugPort>();

    // Build the request
    auto req = debugPort.getEntrypointRequest();
    req.setService(service);
    // Only set entrypoint if one was explicitly provided
    KJ_IF_SOME(e, maybeEntrypoint) {
      req.setEntrypoint(e);
    }
    frankenProps.toCapnp(req.initProps());

    return req.send();
  });

  // Send the request and wrap the result in a Fetcher
  return context.awaitIo(
      js, kj::mv(requestPromise), [&context](jsg::Lock& js, auto result) -> jsg::Ref<api::Fetcher> {
    auto bootstrapCap = result.getEntrypoint();

    // Create a SubrequestChannel that wraps this bootstrap capability
    kj::Own<IoChannelFactory::SubrequestChannel> subrequestChannel =
        kj::refcounted<WorkerdBootstrapSubrequestChannel>(kj::mv(bootstrapCap),
            context.getHttpOverCapnpFactory(), context.getByteStreamFactory());

    return js.alloc<api::Fetcher>(
        context.addObject(kj::mv(subrequestChannel)), api::Fetcher::RequiresHostAndProtocol::NO);
  });
}

jsg::Promise<jsg::Ref<api::Fetcher>> WorkerdDebugPortClient::getActor(
    jsg::Lock& js, kj::String service, kj::String entrypoint, kj::String actorId) {
  auto& context = IoContext::current();

  // Get the debug port capability (async) and then send the request
  auto requestPromise = context.getIoChannelFactory().getWorkerdDebugPort(channel).then(
      [service = kj::mv(service), entrypoint = kj::mv(entrypoint), actorId = kj::mv(actorId)](
          capnp::Capability::Client cap) mutable {
    auto debugPort = cap.castAs<rpc::WorkerdDebugPort>();

    // Build the request
    auto req = debugPort.getActorRequest();
    req.setService(service);
    req.setEntrypoint(entrypoint);
    req.setActorId(actorId);

    return req.send();
  });

  // Send the request and wrap the result in a Fetcher
  return context.awaitIo(
      js, kj::mv(requestPromise), [&context](jsg::Lock& js, auto result) -> jsg::Ref<api::Fetcher> {
    auto bootstrapCap = result.getActor();

    // Create a SubrequestChannel that wraps this bootstrap capability
    kj::Own<IoChannelFactory::SubrequestChannel> subrequestChannel =
        kj::refcounted<WorkerdBootstrapSubrequestChannel>(kj::mv(bootstrapCap),
            context.getHttpOverCapnpFactory(), context.getByteStreamFactory());

    return js.alloc<api::Fetcher>(
        context.addObject(kj::mv(subrequestChannel)), api::Fetcher::RequiresHostAndProtocol::NO);
  });
}

}  // namespace workerd::server
