// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>

#include <workerd/io/worker-interface.capnp.h>
#include <workerd/api/global-scope.h>
#include <workerd/io/worker-interface.h>

namespace workerd::api {

class HibernatableWebSocketEvent final: public ExtendableEvent {
public:
  explicit HibernatableWebSocketEvent();

  static jsg::Ref<HibernatableWebSocketEvent> constructor(kj::String type) = delete;

  // TODO(soon): return correct ws instead of the current stub implementation
  jsg::Ref<WebSocket> getWebSocket(jsg::Lock& lock);
  jsg::Value getError(jsg::Lock& lock);

  JSG_RESOURCE_TYPE(HibernatableWebSocketEvent) {
    JSG_INHERIT(ExtendableEvent);
  }
};

struct HibernatableSocketParams {
  enum Type {
    TEXT,
    DATA,
    CLOSE,
    ERROR
  };

  Type type;
  kj::Array<byte> data;
  kj::String message;
  kj::String closeReason;
  int closeCode;
};

class HibernatableWebSocketCustomEventImpl final: public WorkerInterface::CustomEvent,
    public kj::Refcounted {
public:
  HibernatableWebSocketCustomEventImpl(
      uint16_t typeId,
      kj::TaskSet& waitUntilTasks,
      HibernatableSocketParams params)
    : typeId(typeId), waitUntilTasks(waitUntilTasks), params(kj::mv(params)) {}

  kj::Promise<Result> run(
      kj::Own<IoContext_IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName) override;

  kj::Promise<Result> sendRpc(
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::TaskSet& waitUntilTasks,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

private:
  uint16_t typeId;
  kj::TaskSet& waitUntilTasks;
  HibernatableSocketParams params;
};

#define EW_WEB_SOCKET_MESSAGE_ISOLATE_TYPES      \
  api::HibernatableWebSocketEvent,       \
  api::HibernatableWebSocketExportedHandler
}  // namespace workerd::api
