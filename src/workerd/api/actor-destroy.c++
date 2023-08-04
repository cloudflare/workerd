// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-destroy.h"
#include <workerd/api/global-scope.h>
#include <workerd/jsg/ser.h>

namespace workerd::api {

ActorDestroyEvent::ActorDestroyEvent() : ExtendableEvent("actorDestroy"){};

kj::Promise<WorkerInterface::CustomEvent::Result>
ActorDestroyCustomEventImpl::run(kj::Own<IoContext::IncomingRequest> incomingRequest,
                                 kj::Maybe<kj::StringPtr> entrypointName) {

  auto& context = incomingRequest->getContext();

  // Mark the request as delivered because we're about to run some JS.
  incomingRequest->delivered();

  EventOutcome outcome = EventOutcome::OK;

  try {
    co_return co_await context.run(
        [entrypointName = entrypointName, &context](Worker::Lock& lock) mutable {
          return lock.getGlobalScope().actorDestroy(
              lock, lock.getExportedHandler(entrypointName, context.getActor()));
        });
  } catch (kj::Exception e) {
    if (auto desc = e.getDescription();
        !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
      LOG_EXCEPTION("ActorDestroyCustomEventImpl"_kj, e);
    }
    outcome = EventOutcome::EXCEPTION;
  }

  co_return Result{
      .outcome = outcome,
  };
}

kj::Promise<WorkerInterface::CustomEvent::Result> ActorDestroyCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory, capnp::ByteStreamFactory& byteStreamFactory,
    kj::TaskSet& waitUntilTasks, rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.castAs<rpc::EventDispatcher>().actorDestroyRequest();

  return req.send().then([](auto resp) {
    return WorkerInterface::CustomEvent::Result{
        .outcome = resp.getOutcome(),
    };
  });
}

} // namespace workerd::api
