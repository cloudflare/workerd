// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Classes for calling a remote Worker/Durable Object's methods from the stub over RPC.
// This file contains the generic stub object (JsRpcCapability), as well as classes for sending and
// delivering the RPC event.
//
// `JsRpcCapability` specifically represents a capability that was introduced as part of some
// broader RPC session. `Fetcher`, on the other hand, also supports RPC methods, where each method
// call begins a new session (by dispatching a `jsRpcSession` custom event). Service bindings and
// Durable Object stubs both extend from `Fetcher`, and so allow such calls.
//
// See worker-interface.capnp for the underlying protocol.

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/function.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.capnp.h>

namespace workerd::api {

// For the same reason we limit the size of WebSocket messages to 1MB, we limit RPC payloads.
// Very large messages would both cause problems for the underlying Cap'n Proto transport,
// as well as put too much memory pressure on the isolate. Applications which need to move
// large amounts of data should split the data into several smaller chunks transmitted through
// separate calls.
constexpr size_t MAX_JS_RPC_MESSAGE_SIZE = 1u << 20;

// A JsRpcCapability object forwards JS method calls to the remote Worker/Durable Object over RPC.
// Since methods are not known until runtime, JsRpcCapability doesn't define any JS methods.
// Instead, we use JSG_WILDCARD_PROPERTY to intercept property accesses of names that are not known
// at compile time.
//
// JsRpcCapability only supports method calls. You cannot, for instance, access a property of a
// Durable Object over RPC.
//
// The `JsRpcCapability` type is used to represent capabilities passed across some previous JS RPC
// call. It is NOT the type of a Durable Object stub nor a service binding. Those are instances of
// `Fetcher`, which has a `getRpcMethod()` call of its own that mostly delegates to
// `JsRpcCapability::sendJsRpc()`.
class JsRpcCapability: public jsg::Object {
public:
  JsRpcCapability(IoOwn<rpc::JsRpcTarget::Client> capnpClient)
      : capnpClient(kj::mv(capnpClient)) {}

  // Serializes the method name and arguments, calls customEvent to get the capability, and uses
  // the capability to send our request to the remote Worker. This resolves once the RPC promise
  // resolves.
  //
  // This is a static helper, no `JsRpcCapability` object is needed. This is the shared
  // implementation between `JsRpcCapability::getRpcMethod()` and `Fetcher::getRpcMethod()`.
  static jsg::Promise<jsg::Value> sendJsRpc(
      jsg::Lock& js,
      rpc::JsRpcTarget::Client client,
      kj::StringPtr name,
      const v8::FunctionCallbackInfo<v8::Value>& args);

  using RpcFunction = jsg::Function<jsg::Promise<jsg::Value>(
      const v8::FunctionCallbackInfo<v8::Value>& info)>;

  kj::Maybe<RpcFunction> getRpcMethod(jsg::Lock& js, kj::StringPtr name);

  JSG_RESOURCE_TYPE(JsRpcCapability) {
    JSG_WILDCARD_PROPERTY(getRpcMethod);
  }

private:
  IoOwn<rpc::JsRpcTarget::Client> capnpClient;
};

// `jsRpcSession` returns a capability that provides the client a way to call remote methods
// over RPC. We drain the IncomingRequest after the capability is used to run the relevant JS.
class JsRpcSessionCustomEventImpl final: public WorkerInterface::CustomEvent {
public:
  JsRpcSessionCustomEventImpl(uint16_t typeId,
      kj::PromiseFulfillerPair<rpc::JsRpcTarget::Client> paf =
          kj::newPromiseAndFulfiller<rpc::JsRpcTarget::Client>())
    : capFulfiller(kj::mv(paf.fulfiller)),
      clientCap(kj::mv(paf.promise)),
      typeId(typeId) {}

  kj::Promise<Result> run(
      kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName) override;

  kj::Promise<Result> sendRpc(
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::TaskSet& waitUntilTasks,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

  rpc::JsRpcTarget::Client getCap() { return clientCap; }

  // Event ID for jsRpcSession.
  //
  // Similar to WebSocket hibernation, we define this event ID in the internal codebase, but since
  // we don't create JsRpcSessionCustomEventImpl from our internal code, we can't pass the event
  // type in -- so we hardcode it here.
  static constexpr uint16_t WORKER_RPC_EVENT_TYPE = 9;

private:
  kj::Own<kj::PromiseFulfiller<workerd::rpc::JsRpcTarget::Client>> capFulfiller;

  // We need to set the client/server capability on the event itself to get around CustomEvent's
  // limited return type.
  rpc::JsRpcTarget::Client clientCap;
  uint16_t typeId;

  class ServerTopLevelMembrane;
};

#define EW_WORKER_RPC_ISOLATE_TYPES  \
  api::JsRpcCapability

}; // namespace workerd::api
