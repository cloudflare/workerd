// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Classes for calling a remote Worker/Durable Object's methods from the stub over RPC.
// This file contains the generic stub object (JsRpcStub), as well as classes for sending and
// delivering the RPC event.
//
// `JsRpcStub` specifically represents a capability that was introduced as part of some
// broader RPC session. `Fetcher`, on the other hand, also supports RPC methods, where each method
// call begins a new session (by dispatching a `jsRpcSession` custom event). Service bindings and
// Durable Object stubs both extend from `Fetcher`, and so allow such calls.
//
// See worker-interface.capnp for the underlying protocol.

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
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

// ExternalHandler used when serializing RPC messages. Serialization functions which whish to
// handle RPC specially should use this.
class RpcSerializerExternalHander final: public jsg::Serializer::ExternalHandler {
public:
  using BuilderCallback = kj::Function<void(rpc::JsValue::External::Builder)>;

  // Add an external. The value is a callback which will be invoked later to fill in the
  // JsValue::External in the Cap'n Proto structure. The external array cannot be allocated until
  // the number of externals are known, which is only after all calls to `add()` have completed,
  // hence the need for a callback.
  void write(BuilderCallback callback) { externals.add(kj::mv(callback)); }

  // Build the final list.
  capnp::Orphan<capnp::List<rpc::JsValue::External>> build(capnp::Orphanage orphanage);

  size_t size() { return externals.size(); }

private:
  kj::Vector<BuilderCallback> externals;
};

// ExternalHandler used when deserializing RPC messages. Deserialization functions which whish to
// handle RPC specially should use this.
class RpcDeserializerExternalHander final: public jsg::Deserializer::ExternalHandler {
public:
  RpcDeserializerExternalHander(capnp::List<rpc::JsValue::External>::Reader externals)
      : externals(externals) {}
  ~RpcDeserializerExternalHander() noexcept(false);

  // Read and return the next external.
  rpc::JsValue::External::Reader read();

private:
  capnp::List<rpc::JsValue::External>::Reader externals;
  uint i = 0;

  kj::UnwindDetector unwindDetector;
};

// Base class for objects which can be sent over RPC, but doing so actually sends a stub which
// makes RPCs back to the original object.
class JsRpcTarget: public jsg::Object {
public:
  static jsg::Ref<JsRpcTarget> constructor() { return jsg::alloc<JsRpcTarget>(); }

  JSG_RESOURCE_TYPE(JsRpcTarget) {}

  // Serializes to JsRpcStub.
  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  JSG_ONEWAY_SERIALIZABLE(rpc::SerializationTag::JS_RPC_STUB);
};

// Common superclass of JsRpcStub and Fetcher, the two types that may serve as the basis for
// RPC calls.
//
// This class is NOT part of the JavaScript class heirarchy (it has no JSG_RESOURCE_TYPE block),
// it's only a C++ class used to abstract how to get a capnp client out of the object.
class JsRpcClientProvider: public jsg::Object {
public:
  // Get a capnp client that can be used to dispatch one call.
  //
  // If this isn't the root object (i.e. this is a JsRpcProperty), the property path starting from
  // the root object will be appended to `path`.
  virtual rpc::JsRpcTarget::Client getClientForOneCall(
      jsg::Lock& js, kj::Vector<kj::StringPtr>& path) = 0;
};

// Represents a property -- possibly, a method -- of a remote RPC object.
class JsRpcProperty: public JsRpcClientProvider {
public:
  JsRpcProperty(jsg::Ref<JsRpcClientProvider> parent, kj::String name)
      : parent(kj::mv(parent)), name(kj::mv(name)) {}

  rpc::JsRpcTarget::Client getClientForOneCall(
      jsg::Lock& js, kj::Vector<kj::StringPtr>& path) override;

  // Call the property as a method.
  jsg::Promise<jsg::Value> call(const v8::FunctionCallbackInfo<v8::Value>& args);

  // Get a nested property, using pipelining.
  kj::Maybe<jsg::Ref<JsRpcProperty>> getProperty(jsg::Lock& js, kj::String name);

  JSG_RESOURCE_TYPE(JsRpcProperty) {
    JSG_CALLABLE(call);
    JSG_WILDCARD_PROPERTY(getProperty);
  }

private:
  // The parent object from which this property was obtained.
  jsg::Ref<JsRpcClientProvider> parent;

  // Name of this property within its immediate parent.
  kj::String name;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(parent);
  }
};

// A JsRpcStub object forwards JS method calls to the remote Worker/Durable Object over RPC.
// Since methods are not known until runtime, JsRpcStub doesn't define any JS methods.
// Instead, we use JSG_WILDCARD_PROPERTY to intercept property accesses of names that are not known
// at compile time.
//
// JsRpcStub only supports method calls. You cannot, for instance, access a property of a
// Durable Object over RPC.
//
// The `JsRpcStub` type is used to represent capabilities passed across some previous JS RPC
// call. It is NOT the type of a Durable Object stub nor a service binding. Those are instances of
// `Fetcher`, which has a `getRpcMethod()` call of its own that mostly delegates to
// `JsRpcStub::sendJsRpc()`.
class JsRpcStub: public JsRpcClientProvider {
public:
  JsRpcStub(IoOwn<rpc::JsRpcTarget::Client> capnpClient)
      : capnpClient(kj::mv(capnpClient)) {}

  rpc::JsRpcTarget::Client getClientForOneCall(
      jsg::Lock& js, kj::Vector<kj::StringPtr>& path) override;

  // Given a JsRpcTarget, make an RPC stub from it.
  //
  // Usually, applications won't use this constructor directly. Rather, they will define types
  // that extend `JsRpcTarget` and then they will simply return those. The serializer will
  // automatically handle `JsRpcTarget` by wrapping it in `JsRpcStub`. However, it can be useful
  // for testing to be able to construct a loopback stub.
  static jsg::Ref<JsRpcStub> constructor(jsg::Lock& js, jsg::Ref<JsRpcTarget> object);

  kj::Maybe<jsg::Ref<JsRpcProperty>> getRpcMethod(jsg::Lock& js, kj::String name);

  JSG_RESOURCE_TYPE(JsRpcStub) {
    JSG_WILDCARD_PROPERTY(getRpcMethod);
  }

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<JsRpcStub> deserialize(
      jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer);

  JSG_SERIALIZABLE(rpc::SerializationTag::JS_RPC_STUB);

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

// Base class for exported RPC services.
//
// When the worker's top-level module exports a class that extends this class, it means that it
// is a stateless service.
//
//     import {WorkerEntrypoint} from "cloudflare:workers";
//     export class MyService extends WorkerEntrypoint {
//       async fetch(req) { ... }
//       async someRpcMethod(a, b) { ... }
//     }
//
// `env` and `ctx` are automatically available as `this.env` and `this.ctx`, without the need to
// define a constructor.
class WorkerEntrypoint: public jsg::Object {
public:
  static jsg::Ref<WorkerEntrypoint> constructor(
      const v8::FunctionCallbackInfo<v8::Value>& args,
      jsg::Ref<ExecutionContext> ctx, jsg::JsObject env);

  JSG_RESOURCE_TYPE(WorkerEntrypoint) {}
};

// Like WorkerEntrypoint, but this is the base class for Durable Object classes.
//
// Note that the name of this class as seen by JavaScript is `DurableObject`, but using that name
// in C++ would conflict with the type name currently used by DO stubs.
// TODO(cleanup): Rename DO stubs to `DurableObjectStub`?
//
// Historically, DO classes were not expected to inherit anything. However, this made it impossible
// to tell whether an exported class was intended to be a DO class vs. something else. Originally
// there were no other kinds of exported classes so this was fine. Going forward, we encourage
// everyone to be explicit by inheriting this, and we require it if you want to use RPC.
class DurableObjectBase: public jsg::Object {
public:
  static jsg::Ref<DurableObjectBase> constructor(
      const v8::FunctionCallbackInfo<v8::Value>& args,
      jsg::Ref<DurableObjectState> ctx, jsg::JsObject env);

  JSG_RESOURCE_TYPE(DurableObjectBase) {}
};

// The "cloudflare:workers" module, which exposes the WorkerEntrypoint and DurableObject types
// for extending.
class EntrypointsModule: public jsg::Object {
public:
  JSG_RESOURCE_TYPE(EntrypointsModule) {
    JSG_NESTED_TYPE(WorkerEntrypoint);
    JSG_NESTED_TYPE_NAMED(DurableObjectBase, DurableObject);
    JSG_NESTED_TYPE_NAMED(JsRpcStub, RpcStub);
    JSG_NESTED_TYPE_NAMED(JsRpcTarget, RpcTarget);
  }
};

#define EW_WORKER_RPC_ISOLATE_TYPES  \
  api::JsRpcProperty,                \
  api::JsRpcStub,                    \
  api::JsRpcTarget,                  \
  api::WorkerEntrypoint,             \
  api::DurableObjectBase,            \
  api::EntrypointsModule

template <class Registry>
void registerRpcModules(Registry& registry, CompatibilityFlags::Reader flags) {
  if (flags.getWorkerdExperimental()) {
    registry.template addBuiltinModule<EntrypointsModule>(
        "cloudflare-internal:workers", workerd::jsg::ModuleRegistry::Type::INTERNAL);
  }
}

}; // namespace workerd::api
