// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/worker-rpc.h>
#include <workerd/io/features.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/actor-state.h>
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

jsg::Promise<jsg::Value> JsRpcCapability::sendJsRpc(
    jsg::Lock& js,
    rpc::JsRpcTarget::Client client,
    kj::StringPtr name,
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto& ioContext = IoContext::current();

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

  return ioContext.awaitIo(js, kj::mv(callResult),
      [](jsg::Lock& js, auto result) -> jsg::Value {
    return jsg::Value(js.v8Isolate, deserializeV8(js, result.getResult().getV8Serialized()));
  });
}

kj::Maybe<JsRpcCapability::RpcFunction> JsRpcCapability::getRpcMethod(
    jsg::Lock& js, kj::StringPtr name) {
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

    return sendJsRpc(js, *self->capnpClient, methodName, args);
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

      auto handler = KJ_REQUIRE_NONNULL(lock.getExportedHandler(entrypointName, ctx.getActor()),
                                        "Failed to get handler to worker.");
      auto handle = handler->self.getHandle(lock);

      if (handler->missingSuperclass) {
        // JS RPC is not enabled on the server side, we cannot call any methods.
        JSG_REQUIRE(FeatureFlags::get(js).getJsRpc(), TypeError,
            "The receiving Durable Object does not support RPC, because its class was not declared "
            "with `extends DurableObject`. In order to enable RPC, make sure your class "
            "extends the special class `DurableObject`, which can be imported from the module "
            "\"cloudflare:workers\".");
      }

      // `handler->ctx` is present when we're invoking a freestanding function, and therefore
      // `env` and `ctx` need to be passed as parameters. In that case, we our method lookup
      // should obviously permit instance properties, since we expect the export is a plain object.
      // Otherwise, though, the export is a class. In that case, we have set the rule that we will
      // only allow class properties (aka prototype properties) to be accessed, to avoid
      // programmers shooting themselves in the foot by forgetting to make their members private.
      bool allowInstanceProperties = handler->ctx != kj::none;

      // We will try to get the function, if we can't we'll throw an error to the client.
      auto fn = tryGetFn(lock, ctx, handle, methodName, allowInstanceProperties);

      v8::Local<v8::Value> invocationResult;
      KJ_IF_SOME(execCtx, handler->ctx) {
        invocationResult = invokeFnInsertingEnvCtx(js, fn, handle, serializedArgs,
            handler->env.getHandle(js),
            lock.getWorker().getIsolate().getApi().wrapExecutionContext(js, execCtx.addRef()));
      } else {
        invocationResult = invokeFn(js, fn, handle, serializedArgs);
      }

      // We have a function, so let's call it and serialize the result for RPC.
      // If the function returns a promise we will wait for the promise to finish so we can
      // serialize the result.
      return ctx.awaitJs(js, js.toPromise(invocationResult)
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
        name == "webSocketError" ||
        // All JS classes define a method `constructor` on the prototype, but we don't actually
        // want this to be callable over RPC!
        name == "constructor") {
      return true;
    }
    return false;
  }

  // If the `methodName` is a known public method, we'll return it.
  inline v8::Local<v8::Function> tryGetFn(
      Worker::Lock& lock,
      IoContext& ctx,
      v8::Local<v8::Object> handle,
      kj::StringPtr methodName,
      bool allowInstanceProperties) {
    jsg::Lock& js(lock);

    if (!allowInstanceProperties) {
      auto proto = handle->GetPrototype();
      // This assert can't fail because we only take this branch when operating on a class
      // instance.
      KJ_ASSERT(proto->IsObject());
      handle = proto.As<v8::Object>();
    }

    auto methodStr = jsg::v8StrIntern(lock.getIsolate(), methodName);
    auto fnHandle = jsg::check(handle->Get(lock.getContext(), methodStr));

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

  // Like `invokeFn`, but inject the `env` and `ctx` values between the first and second
  // parameters. Used for service bindings that use functional syntax.
  v8::Local<v8::Value> invokeFnInsertingEnvCtx(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      v8::Local<v8::Object> thisArg,
      kj::ArrayPtr<const kj::byte> serializedArgs,
      v8::Local<v8::Value> env,
      jsg::JsObject ctx) {
    // Determine the function arity (how many parameters it was declared to accept) by reading the
    // `.length` attribute.
    auto arity = js.withinHandleScope([&]() {
      auto length = jsg::check(fn->Get(js.v8Context(), js.strIntern("length")));
      return jsg::check(length->IntegerValue(js.v8Context()));
    });

    // Avoid excessive allocation from a maliciously-set `length`.
    JSG_REQUIRE(arity >= 0 && arity < 256, TypeError,
        "RPC function has unreasonable length attribute: ", arity);

    if (arity < 3) {
      // If a function has fewer than three arguments, assume that env or ctx was omitted, since
      // historical handlers that existed before RPC worked that way.
      arity = 3;
    }

    // We're going to pass all the arguments from the client to the function, but we are going to
    // insert `env` and `ctx`. We assume the last two arguments that the function declared are
    // `env` and `ctx`, so we can determine where to insert them based on the function's arity.
    kj::Maybe<jsg::JsArray> argsArrayFromClient;
    size_t argCountFromClient = 0;
    if (serializedArgs.size() > 0) {
      auto array = KJ_REQUIRE_NONNULL(
          deserializeV8(js, serializedArgs).tryCast<jsg::JsArray>(),
          "expected JsArray when deserializing arguments.");
      argCountFromClient = array.size();
      argsArrayFromClient = kj::mv(array);
    }

    KJ_STACK_ARRAY(v8::Local<v8::Value>, arguments, kj::max(argCountFromClient + 2, arity), 8, 8);

    for (auto i: kj::zeroTo(arity - 2)) {
      if (argCountFromClient > i) {
        arguments[i] = KJ_ASSERT_NONNULL(argsArrayFromClient).get(js, i);
      } else {
        arguments[i] = js.undefined();
      }
    }

    arguments[arity - 2] = env;
    arguments[arity - 1] = ctx;

    KJ_IF_SOME(a, argsArrayFromClient) {
      for (size_t i = arity - 2; i < argCountFromClient; ++i) {
        arguments[i + 2] = a.get(js, i);
      }
    }

    return jsg::check(fn->Call(js.v8Context(), thisArg, arguments.size(), arguments.begin()));
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
  // collector to actually collect the JsRpcCapability objects.
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

// =======================================================================================

jsg::Ref<WorkerEntrypoint> WorkerEntrypoint::constructor(
    jsg::Ref<ExecutionContext> ctx, jsg::JsObject env) {
  return jsg::alloc<WorkerEntrypoint>();
}
jsg::Ref<DurableObjectBase> DurableObjectBase::constructor(
    jsg::Ref<DurableObjectState> ctx, jsg::JsObject env) {
  return jsg::alloc<DurableObjectBase>();
}

}; // namespace workerd::api
