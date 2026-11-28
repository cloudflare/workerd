// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>

namespace workerd {

// Delivers a Durable Object's onHibernate hook. See Worker::Actor::runOnHibernate() for how the
// WorkerInterface that receives it must be built.
class OnHibernateCustomEvent final: public WorkerInterface::CustomEvent {
 public:
  explicit OnHibernateCustomEvent(Worker::Actor::HibernationReason reason): reason(reason) {}

  static constexpr uint16_t TYPE = 15;

  kj::Promise<Result> run(kj::Own<IoContext_IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      kj::Maybe<Worker::VersionInfo> versionInfo,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks,
      bool isDynamicDispatch) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      FrankenvalueHandler& frankenvalueHandler,
      rpc::EventDispatcher::Client dispatcher) override;

  kj::Promise<Result> notSupported() override {
    KJ_UNIMPLEMENTED("onHibernate event not supported");
  }

  uint16_t getType() override {
    return TYPE;
  }

  tracing::EventInfo getEventInfo() const override {
    return tracing::OnHibernateEventInfo(reason);
  }

 private:
  Worker::Actor::HibernationReason reason;
};

}  // namespace workerd
