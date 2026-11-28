// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "on-hibernate-event.h"

#include <workerd/io/io-context.h>

namespace workerd {

kj::Promise<WorkerInterface::CustomEvent::Result> OnHibernateCustomEvent::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks,
    bool isDynamicDispatch) {
  auto& actor = KJ_REQUIRE_NONNULL(
      incomingRequest->getContext().getActor(), "onHibernate events can only reach actors");
  auto outcome = co_await actor.runOnHibernate(kj::mv(incomingRequest), reason);
  co_return Result{.outcome = outcome};
}

kj::Promise<WorkerInterface::CustomEvent::Result> OnHibernateCustomEvent::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    FrankenvalueHandler& frankenvalueHandler,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.onHibernateRequest(capnp::MessageSize{4, 0});
  req.setReason(reason);
  auto response = co_await req.send();
  co_return Result{.outcome = response.getResult()};
}

}  // namespace workerd
