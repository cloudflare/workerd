// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/worker-rpc.h>
#include <workerd/io/features.h>
#include <workerd/api/global-scope.h>
#include <workerd/jsg/ser.h>
#include <capnp/membrane.h>

namespace workerd::api {

namespace {

kj::Array<kj::byte> serializeV8(jsg::Lock& js, jsg::JsValue value) {
  jsg::Serializer serializer(js, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(js, value);
  return serializer.release().data;
}

jsg::JsValue deserializeV8(jsg::Lock& js, kj::ArrayPtr<const kj::byte> ser) {
  jsg::Deserializer deserializer(js, ser, kj::none, kj::none,
      jsg::Deserializer::Options {
    .version = 15,
    .readHeader = true,
  });

  return deserializer.readValue(js);
}

// A membrane applied which detects when no capabilities are held any longer, at which point it
// fulfills a fulfiller.
//
// TODO(cleanup): This is generally useful, should it be part of capnp?
class CompletionMembrane final: public capnp::MembranePolicy, public kj::Refcounted {
public:
  explicit CompletionMembrane(kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : doneFulfiller(kj::mv(doneFulfiller)) {}
  ~CompletionMembrane() noexcept(false) {
    doneFulfiller->fulfill();
  }

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }

  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }

  kj::Own<MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }

private:
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;
};

// A membrane which revokes when some Promise is fulfilled.
//
// TODO(cleanup): This is generally useful, should it be part of capnp?
class RevokerMembrane final: public capnp::MembranePolicy, public kj::Refcounted {
public:
  explicit RevokerMembrane(kj::Promise<void> promise)
      : promise(promise.fork()) {}

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }

  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }

  kj::Own<MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }

  kj::Maybe<kj::Promise<void>> onRevoked() override {
    return promise.addBranch();
  }

private:
  kj::ForkedPromise<void> promise;
};

} // namespace

jsg::Promise<jsg::Value> WorkerRpc::sendWorkerRpc(
    jsg::Lock& js,
    kj::StringPtr name,
    const v8::FunctionCallbackInfo<v8::Value>& args) {

  auto& ioContext = IoContext::current();
  auto worker = getClient(ioContext, kj::none, "jsRpcSession"_kjc);
  auto event = kj::heap<api::JsRpcSessionCustomEventImpl>(WORKER_RPC_EVENT_TYPE);

  rpc::JsRpcTarget::Client client = event->getCap();
  auto builder = client.callRequest();
  builder.setMethodName(name);

  kj::Vector<jsg::JsValue> argv(args.Length());
  for (int n = 0; n < args.Length(); n++) {
    argv.add(jsg::JsValue(args[n]));
  }

  // If we have arguments, serialize them.
  // Note that we may fail to serialize some element, in which case this will throw back to JS.
  if (argv.size() > 0) {
    // TODO(perf): It would be nice if we could serialize directly into the capnp message to avoid
    // a redundant copy of the bytes here. Maybe we could even cancel serialization early if it
    // goes over the size limit.
    auto ser = serializeV8(js, js.arr(argv.asPtr()));
    JSG_ASSERT(ser.size() <= MAX_JS_RPC_MESSAGE_SIZE, Error,
        "Serialized RPC requests are limited to 1MiB, but the size of this request was: ",
        ser.size(), " bytes.");
    builder.initArgs().setV8Serialized(ser);
  }

  auto callResult = builder.send();
  auto customEventResult = worker->customEvent(kj::mv(event)).attach(kj::mv(worker));

  // If customEvent throws, we'll cancel callResult and propagate the exception. Otherwise, we'll
  // just wait until callResult finishes.
  //
  // Note: `callResult` does not depend on the `WorkerRpc` object staying live! This is important
  // as `sendWorkerRpc()` is documented as returning a Promise that is allowed to outlive the
  // `WorkerRpc` object itself.
  auto promise = callResult.exclusiveJoin(customEventResult
      .then([](auto&&) -> kj::Promise<capnp::Response<rpc::JsRpcTarget::CallResults>> {
        return kj::NEVER_DONE;
      }));

  return ioContext.awaitIo(js, kj::mv(promise),
      [](jsg::Lock& js, auto result) -> jsg::Value {
    return jsg::Value(js.v8Isolate, deserializeV8(js, result.getResult().getV8Serialized()));
  });
}

kj::Maybe<WorkerRpc::RpcFunction> WorkerRpc::getRpcMethod(jsg::Lock& js, kj::StringPtr name) {
  return RpcFunction(JSG_VISITABLE_LAMBDA(
      (methodName = kj::str(name), self = JSG_THIS),
      (self),
      (jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) -> jsg::Promise<jsg::Value> {
    // Let's make sure that the returned function wasn't invoked using a different `this` than the
    // one intended. We don't really _have to_ check this, but if we don't, then we have to forever
    // support tearing RPC methods off their owning objects and invoking them elsewhere. Enforcing
    // it might allow future implementation flexibility. (We use the terrible error message
    // "Illegal invocation" because this is the exact message V8 normally generates when invoking a
    // native function with the wrong `this`.)
    //
    // TODO(cleanup): Ideally, we'd actually obtain `self` by unwrapping `args.This()` to a
    // `jsg::Ref<JsRpcCapability>` instead of capturing it, but there isn't a JSG-blessed way to do
    // that.
    JSG_REQUIRE(args.This() == KJ_ASSERT_NONNULL(self.tryGetHandle(js)), TypeError,
        "Illegal invocation");

    return self->sendWorkerRpc(js, methodName, args);
  }));
}

// The capability that lets us call remote methods over RPC.
// The client capability is dropped after each callRequest().
class JsRpcTargetImpl final : public rpc::JsRpcTarget::Server {
public:
  JsRpcTargetImpl(
      IoContext& ctx,
      kj::Maybe<kj::StringPtr> entrypointName)
      : ctx(ctx), entrypointName(entrypointName) {}

  // Handles the delivery of JS RPC method calls.
  kj::Promise<void> call(CallContext callContext) override {
    auto methodName = kj::heapString(callContext.getParams().getMethodName());
    auto serializedArgs = callContext.getParams().getArgs().getV8Serialized().asBytes();

    // Try to execute the requested method.
    co_return co_await ctx.run(
        [this, methodName=kj::mv(methodName), serializedArgs=kj::mv(serializedArgs), callContext]
        (Worker::Lock& lock) mutable -> kj::Promise<void> {

      jsg::Lock& js = lock;
      // JS RPC is not enabled on the server side, we cannot call any methods.
      JSG_REQUIRE(FeatureFlags::get(js).getJsRpc(), TypeError,
          "The receiving Worker does not allow its methods to be called over RPC.");

      auto& handler = KJ_REQUIRE_NONNULL(lock.getExportedHandler(entrypointName, ctx.getActor()),
                                         "Failed to get handler to worker.");
      auto handle = handler.self.getHandle(lock);

      // We will try to get the function, if we can't we'll throw an error to the client.
      auto fn = tryGetFn(lock, ctx, handle, methodName);

      // We have a function, so let's call it and serialize the result for RPC.
      // If the function returns a promise we will wait for the promise to finish so we can
      // serialize the result.
      return ctx.awaitJs(js, js.toPromise(invokeFn(js, fn, handle, serializedArgs))
          .then(js, ctx.addFunctor([callContext](jsg::Lock& js, jsg::Value value) mutable {
        auto result = serializeV8(js, jsg::JsValue(value.getHandle(js)));
        JSG_ASSERT(result.size() <= MAX_JS_RPC_MESSAGE_SIZE, Error,
            "Serialized RPC responses are limited to 1MiB, but the size of this response was: ",
            result.size(), " bytes.");
        auto builder = callContext.initResults(capnp::MessageSize { result.size() / 8 + 8, 0 });
        builder.initResult().setV8Serialized(kj::mv(result));
      })));
    });
  }

  KJ_DISALLOW_COPY_AND_MOVE(JsRpcTargetImpl);

private:
  // The following names are reserved by the Workers Runtime and cannot be called over RPC.
  bool isReservedName(kj::StringPtr name) {
    if (name == "fetch" ||
        name == "connect" ||
        name == "alarm" ||
        name == "webSocketMessage" ||
        name == "webSocketClose" ||
        name == "webSocketError") {
      return true;
    }
    return false;
  }

  // If the `methodName` is a known public method, we'll return it.
  inline v8::Local<v8::Function> tryGetFn(
      Worker::Lock& lock,
      IoContext& ctx,
      v8::Local<v8::Object> handle,
      kj::StringPtr methodName) {
    auto methodStr = jsg::v8StrIntern(lock.getIsolate(), methodName);
    auto fnHandle = jsg::check(handle->Get(lock.getContext(), methodStr));

    jsg::Lock& js(lock);
    v8::Local<v8::Object> obj = js.obj();
    auto objProto = obj->GetPrototype().As<v8::Object>();

    // Get() will check the Object and the prototype chain. We want to verify that the function
    // we intend to call is not the one defined on the Object prototype.
    bool isImplemented = fnHandle != jsg::check(objProto->Get(js.v8Context(), methodStr));

    JSG_REQUIRE(isImplemented && fnHandle->IsFunction(), TypeError,
        kj::str("The RPC receiver does not implement the method \"", methodName, "\"."));
    JSG_REQUIRE(!isReservedName(methodName), TypeError,
        kj::str("'", methodName, "' is a reserved method and cannot be called over RPC."));
    return fnHandle.As<v8::Function>();
  }

  // Deserializes the arguments and passes them to the given function.
  v8::Local<v8::Value> invokeFn(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      v8::Local<v8::Object> thisArg,
      kj::ArrayPtr<const kj::byte> serializedArgs) {
    // We received arguments from the client, deserialize them back to JS.
    if (serializedArgs.size() > 0) {
      auto args = KJ_REQUIRE_NONNULL(
          deserializeV8(js, serializedArgs).tryCast<jsg::JsArray>(),
          "expected JsArray when deserializing arguments.");
      // Call() expects a `Local<Value> []`... so we populate an array.
      KJ_STACK_ARRAY(v8::Local<v8::Value>, arguments, args.size(), 8, 8);
      for (size_t i = 0; i < args.size(); ++i) {
        arguments[i] = args.get(js, i);
      }
      return jsg::check(fn->Call(js.v8Context(), thisArg, args.size(), arguments.begin()));
    } else {
      return jsg::check(fn->Call(js.v8Context(), thisArg, 0, nullptr));
    }
  };

  IoContext& ctx;
  kj::Maybe<kj::StringPtr> entrypointName;
};

// A membrane which wraps the top-level JsRpcTarget of an RPC session on the server side. The
// purpose of this membrane is to allow only a single top-level call, which then gets a
// `CompletionMembrane` wrapped around it. Note that we can't just wrap `CompletionMembrane` around
// the top-level object directly because that capability will not be dropped until the RPC session
// completes, since it is actually returned as the result of the top-level RPC call, but that
// call doesn't return until the `CompletionMembrane` says all capabilities were dropped, so this
// would create a cycle.
class JsRpcSessionCustomEventImpl::ServerTopLevelMembrane final
    : public capnp::MembranePolicy, public kj::Refcounted {
public:
  explicit ServerTopLevelMembrane(kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : doneFulfiller(kj::mv(doneFulfiller)) {}
  ~ServerTopLevelMembrane() noexcept(false) {
    KJ_IF_SOME(f, doneFulfiller) {
      f->reject(KJ_EXCEPTION(DISCONNECTED,
          "JS RPC session canceled without calling an RPC method."));
    }
  }

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    auto f = kj::mv(JSG_REQUIRE_NONNULL(doneFulfiller,
        Error, "Only one RPC method call is allowed on this object."));
    doneFulfiller = kj::none;
    return capnp::membrane(kj::mv(target), kj::refcounted<CompletionMembrane>(kj::mv(f)));
  }

  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    KJ_FAIL_ASSERT("ServerTopLevelMembrane shouldn't have outgoing capabilities");
  }

  kj::Own<MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }

private:
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> doneFulfiller;
};

kj::Promise<WorkerInterface::CustomEvent::Result> JsRpcSessionCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName) {
  incomingRequest->delivered();
  auto [donePromise, doneFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(
      capnp::membrane(
          kj::heap<JsRpcTargetImpl>(incomingRequest->getContext(), entrypointName),
          kj::refcounted<ServerTopLevelMembrane>(kj::mv(doneFulfiller))));

  // `donePromise` resolves once there are no longer any capabilities pointing between the client
  // and server as part of this session.
  co_await donePromise;
  co_await incomingRequest->drain();
  co_return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}

kj::Promise<WorkerInterface::CustomEvent::Result>
  JsRpcSessionCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    kj::TaskSet& waitUntilTasks,
    rpc::EventDispatcher::Client dispatcher) {
  // We arrange to revoke all capabilities in this session as soon as `sendRpc()` completes or is
  // canceled. Normally, the server side doesn't return if any capabilities still exist, so this
  // only makes a difference in the case that some sort of an error occurred. We don't strictly
  // have to revoke the capabilities as they are probably already broken anyway, but revoking them
  // helps to ensure that the underlying transport isn't "held open" waiting for the JS garbage
  // collector to actually collect the WorkerRpc objects.
  auto revokePaf = kj::newPromiseAndFulfiller<void>();

  KJ_DEFER({
    if (revokePaf.fulfiller->isWaiting()) {
      revokePaf.fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "JS-RPC session canceled"));
    }
  });

  auto req = dispatcher.jsRpcSessionRequest();
  auto sent = req.send();

  this->capFulfiller->fulfill(
      capnp::membrane(
          sent.getTopLevel(),
          kj::refcounted<RevokerMembrane>(kj::mv(revokePaf.promise))));

  try {
    co_await sent;
  } catch (...) {
    auto e = kj::getCaughtExceptionAsKj();
    if (revokePaf.fulfiller->isWaiting()) {
      revokePaf.fulfiller->reject(kj::cp(e));
    }
    kj::throwFatalException(kj::mv(e));
  }

  co_return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}
}; // namespace workerd::api
