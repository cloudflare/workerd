// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>

#include <workerd/api/basics.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/worker-interface.h>

namespace workerd::api {

class ActorDestroyEvent final : public ExtendableEvent {
public:
  explicit ActorDestroyEvent();

  static jsg::Ref<ActorDestroyEvent> constructor(kj::String type) = delete;

  JSG_RESOURCE_TYPE(ActorDestroyEvent) {
    JSG_INHERIT(ExtendableEvent);
  }
};

class ActorDestroyCustomEventImpl final : public WorkerInterface::CustomEvent,
                                          public kj::Refcounted {
public:
  ActorDestroyCustomEventImpl(uint16_t typeId) : typeId(typeId) {}

  kj::Promise<Result> run(kj::Own<IoContext_IncomingRequest> incomingRequest,
                          kj::Maybe<kj::StringPtr> entrypointName) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
                              capnp::ByteStreamFactory& byteStreamFactory,
                              kj::TaskSet& waitUntilTasks,
                              rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

private:
  uint16_t typeId;
};

} // namespace workerd::api
