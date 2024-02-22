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

capnp::Orphan<capnp::List<rpc::JsValue::External>>
    RpcSerializerExternalHander::build(capnp::Orphanage orphanage) {
  auto result = orphanage.newOrphan<capnp::List<rpc::JsValue::External>>(externals.size());
  auto builder = result.get();
  for (auto i: kj::indices(externals)) {
    externals[i](builder[i]);
  }
  return result;
}

RpcDeserializerExternalHander::~RpcDeserializerExternalHander() noexcept(false) {
  if (!unwindDetector.isUnwinding()) {
    KJ_ASSERT(i == externals.size(), "deserialization did not consume all of the externals");
  }
}

rpc::JsValue::External::Reader RpcDeserializerExternalHander::read() {
  KJ_ASSERT(i < externals.size());
  return externals[i++];
}

namespace {

// Call to construct an `rpc::JsValue` from a JS value.
//
// `makeBuilder` is a function which takes a capnp::MessageSize hint and returns the
// rpc::JsValue::Builder to fill in.
template <typename Func>
void serializeJsValue(jsg::Lock& js, jsg::JsValue value, Func makeBuilder) {
  RpcSerializerExternalHander externalHandler;

  jsg::Serializer serializer(js, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
    .externalHandler = externalHandler,
  });
  serializer.write(js, value);
  kj::Array<const byte> data = serializer.release().data;
  JSG_ASSERT(data.size() <= MAX_JS_RPC_MESSAGE_SIZE, Error,
      "Serialized RPC arguments or return values are limited to 1MiB, but the size of this value "
      "was: ", data.size(), " bytes.");

  capnp::MessageSize hint {0, 0};
  hint.wordCount += (data.size() + sizeof(capnp::word) - 1) / sizeof(capnp::word);
  hint.wordCount += capnp::sizeInWords<rpc::JsValue>();
  hint.wordCount += externalHandler.size() * capnp::sizeInWords<rpc::JsValue::External>();
  hint.capCount += externalHandler.size();

  rpc::JsValue::Builder builder = makeBuilder(hint);

  // TODO(perf): It would be nice if we could serialize directly into the capnp message to avoid
  // a redundant copy of the bytes here. Maybe we could even cancel serialization early if it
  // goes over the size limit.
  builder.setV8Serialized(data);

  if (externalHandler.size() > 0) {
    builder.adoptExternals(externalHandler.build(
        capnp::Orphanage::getForMessageContaining(builder)));
  }
}

// Call to construct a JS value from an `rpc::JsValue`.
jsg::JsValue deserializeJsValue(jsg::Lock& js, rpc::JsValue::Reader reader) {
  RpcDeserializerExternalHander externalHandler(reader.getExternals());

  jsg::Deserializer deserializer(js, reader.getV8Serialized(), kj::none, kj::none,
      jsg::Deserializer::Options {
    .version = 15,
    .readHeader = true,
    .externalHandler = externalHandler,
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

jsg::Promise<jsg::Value> JsRpcProperty::call(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  // Note: We used to enforce that RPC methods had to be called with the correct `this`. That is,
  // we prevented people from doing:
  //
  //   let obj = {foo: someRpcStub.foo};
  //   obj.foo();
  //
  // This would throw "Illegal invocation", as is the norm when pulling methods of a native object.
  // That worked as long as RPC methods were implemented as `jsg::Function`. However, when we
  // switched to RPC methods being implemented as callable objects (JsRpcProperty), this became
  // impossible, because V8's SetCallAsFunctionHandler() arranges that `this` is bound to the
  // callable object itself, regardless of how it was invoked. So now we cannot detect the
  // situation above, because V8 never tells us about `obj` at all.
  //
  // Oh well. It's not a big deal. Just annoying that we have to forever support tearing RPC
  // methods off their source object, even if we change implementations to something where that's
  // less convenient.

  auto client = parent->getClientForOneCall(js);

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
    serializeJsValue(js, js.arr(argv.asPtr()), [&](capnp::MessageSize hint) {
      // TODO(perf): Actually use the size hint.
      return builder.initArgs();
    });
  }

  auto callResult = builder.send();

  return ioContext.awaitIo(js, kj::mv(callResult),
      [](jsg::Lock& js, auto result) -> jsg::Value {
    return jsg::Value(js.v8Isolate, deserializeJsValue(js, result.getResult()));
  });
}

rpc::JsRpcTarget::Client JsRpcStub::getClientForOneCall(jsg::Lock& js) {
  return *capnpClient;
}

kj::Maybe<jsg::Ref<JsRpcProperty>> JsRpcStub::getRpcMethod(
    jsg::Lock& js, kj::StringPtr name) {
  // Do not return a method for `then`, otherwise JavaScript decides this is a thenable, i.e. a
  // custom Promise, which will mean a Promise that resolves to this object will attempt to chain
  // with it, which is not what you want!
  if (name == "then"_kj) return kj::none;

  return jsg::alloc<JsRpcProperty>(JSG_THIS, kj::str(name));
}

void JsRpcStub::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "Remote RPC references can only be serialized for RPC.");
  auto externalHandler = dynamic_cast<RpcSerializerExternalHander*>(&handler);
  JSG_REQUIRE(externalHandler != nullptr, DOMDataCloneError,
      "Remote RPC references can only be serialized for RPC.");

  externalHandler->write([cap = *capnpClient](rpc::JsValue::External::Builder builder) mutable {
    builder.setRpcTarget(kj::mv(cap));
  });
}

jsg::Ref<JsRpcStub> JsRpcStub::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  auto& handler = KJ_REQUIRE_NONNULL(deserializer.getExternalHandler(),
      "got JsRpcStub on non-RPC serialized object?");
  auto externalHandler = dynamic_cast<RpcDeserializerExternalHander*>(&handler);
  KJ_REQUIRE(externalHandler != nullptr, "got JsRpcStub on non-RPC serialized object?");

  auto reader = externalHandler->read();
  KJ_REQUIRE(reader.isRpcTarget(), "external table slot type doesn't match serialization tag");

  auto& ioctx = IoContext::current();
  return jsg::alloc<JsRpcStub>(ioctx.addObject(kj::heap(reader.getRpcTarget())));
}

// Callee-side implementation of JsRpcTarget.
//
// Most of the implementation is in this base class. There are subclasses specializing for the case
// of a top-level entrypoint vs. a transient object introduced by a previous RPC in the same
// session.
class JsRpcTargetBase: public rpc::JsRpcTarget::Server {
public:
  JsRpcTargetBase(IoContext& ctx)
      : weakIoContext(ctx.getWeakRef()) {}

  struct EnvCtx {
    v8::Local<v8::Value> env;
    jsg::JsObject ctx;
  };

  struct TargetInfo {
    // The object on which the RPC method should be invoked.
    v8::Local<v8::Object> target;

    // If `env` and `ctx` need to be delivered as arguments to the method, these are the values
    // to deliver.
    kj::Maybe<EnvCtx> envCtx;
  };

  // Get the object on which the method is to be invoked. This is virtual so that we can have
  // separate subclasses handling the case of an entrypoint vs. a transient RPC object.
  virtual TargetInfo getTargetInfo(Worker::Lock& lock, IoContext& ioCtx) = 0;

  // Handles the delivery of JS RPC method calls.
  kj::Promise<void> call(CallContext callContext) override {
    IoContext& ctx = JSG_REQUIRE_NONNULL(weakIoContext->tryGet(), Error,
        "The destination object for this RPC no longer exists.");

    auto params = callContext.getParams();
    auto methodName = kj::heapString(params.getMethodName());
    kj::Maybe<rpc::JsValue::Reader> args;
    if (params.hasArgs()) {
      args = params.getArgs();
    }

    // HACK: Cap'n Proto call contexts are documented as being pointer-like types where the backing
    // object's lifetime is that of the RPC call, but in reality they are refcounted under the
    // hood. Since well be executing the call in the JS microtask queue, we have no ability to
    // actually cancel execution if a cancellation arrives over RPC, and at the end of that
    // execution we're going to accell the call context to write the results. We could invent some
    // complicated way to skip initializing results in the case the call has been canceled, but
    // it's easier and safer to just grap a refcount on the call context object itself, which
    // fully protects us. So... do that.
    auto ownCallContext = capnp::CallContextHook::from(callContext).addRef();

    // Try to execute the requested method.
    auto promise = ctx.run(
        [this,&ctx,methodName=kj::mv(methodName), args, callContext,
         ownCallContext = kj::mv(ownCallContext), ownThis = thisCap()]
        (Worker::Lock& lock) mutable -> kj::Promise<void> {

      jsg::Lock& js = lock;

      auto targetInfo = getTargetInfo(lock, ctx);

      // `targetInfo.envCtx` is present when we're invoking a freestanding function, and therefore
      // `env` and `ctx` need to be passed as parameters. In that case, we our method lookup
      // should obviously permit instance properties, since we expect the export is a plain object.
      // Otherwise, though, the export is a class. In that case, we have set the rule that we will
      // only allow class properties (aka prototype properties) to be accessed, to avoid
      // programmers shooting themselves in the foot by forgetting to make their members private.
      bool allowInstanceProperties = targetInfo.envCtx != kj::none;

      // We will try to get the function, if we can't we'll throw an error to the client.
      auto fn = tryGetFn(lock, ctx, targetInfo.target, methodName, allowInstanceProperties);

      v8::Local<v8::Value> invocationResult;
      KJ_IF_SOME(envCtx, targetInfo.envCtx) {
        invocationResult = invokeFnInsertingEnvCtx(js, methodName, fn, targetInfo.target, args,
            envCtx.env, envCtx.ctx);
      } else {
        invocationResult = invokeFn(js, fn, targetInfo.target, args);
      }

      // We have a function, so let's call it and serialize the result for RPC.
      // If the function returns a promise we will wait for the promise to finish so we can
      // serialize the result.
      return ctx.awaitJs(js, js.toPromise(invocationResult)
          .then(js, ctx.addFunctor(
              [callContext, ownCallContext = kj::mv(ownCallContext)]
              (jsg::Lock& js, jsg::Value value) mutable {
        serializeJsValue(js, jsg::JsValue(value.getHandle(js)), [&](capnp::MessageSize hint) {
          hint.wordCount += capnp::sizeInWords<CallResults>();
          return callContext.initResults(hint).initResult();
        });
      })));
    });

    // We need to make sure this RPC is canceled if the IoContext is destroyed. To accomplish that,
    // we add the promise as a task on the context itself, and use a separate promise fulfiller to
    // wait on the result.
    auto paf = kj::newPromiseAndFulfiller<void>();
    promise = promise.then([&fulfiller=*paf.fulfiller]() {
      fulfiller.fulfill();
    }, [&fulfiller=*paf.fulfiller](kj::Exception&& e) {
      fulfiller.reject(kj::mv(e));
    });
    promise = promise.attach(kj::defer([fulfiller = kj::mv(paf.fulfiller)]() mutable {
      if (fulfiller->isWaiting()) {
        fulfiller->reject(JSG_KJ_EXCEPTION(FAILED, Error,
            "jsg.Error: The destination execution context for this RPC was canceled while the "
            "call was still running."));
      }
    }));
    ctx.addTask(kj::mv(promise));

    return kj::mv(paf.promise);
  }

  KJ_DISALLOW_COPY_AND_MOVE(JsRpcTargetBase);

private:
  // The following names are reserved by the Workers Runtime and cannot be called over RPC.
  static bool isReservedName(kj::StringPtr name) {
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
  static inline v8::Local<v8::Function> tryGetFn(
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

    JSG_REQUIRE(isImplemented, TypeError,
        kj::str("The RPC receiver does not implement the method \"", methodName, "\"."));
    JSG_REQUIRE(fnHandle->IsFunction(), TypeError,
        kj::str("\"", methodName, "\" is not a function."));
    JSG_REQUIRE(!isReservedName(methodName), TypeError,
        kj::str("'", methodName, "' is a reserved method and cannot be called over RPC."));
    return fnHandle.As<v8::Function>();
  }

  // Deserializes the arguments and passes them to the given function.
  static v8::Local<v8::Value> invokeFn(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      v8::Local<v8::Object> thisArg,
      kj::Maybe<rpc::JsValue::Reader> args) {
    // We received arguments from the client, deserialize them back to JS.
    KJ_IF_SOME(a, args) {
      auto args = KJ_REQUIRE_NONNULL(
          deserializeJsValue(js, a).tryCast<jsg::JsArray>(),
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
  static v8::Local<v8::Value> invokeFnInsertingEnvCtx(
      jsg::Lock& js,
      kj::StringPtr methodName,
      v8::Local<v8::Function> fn,
      v8::Local<v8::Object> thisArg,
      kj::Maybe<rpc::JsValue::Reader> args,
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
      // If a function has fewer than three arguments, reproduce the historical behavior where
      // we'd pass the main argument followed by `env` and `ctx` and the undeclared parameters
      // would just be truncated.
      arity = 3;
    }

    // We're going to pass all the arguments from the client to the function, but we are going to
    // insert `env` and `ctx`. We assume the last two arguments that the function declared are
    // `env` and `ctx`, so we can determine where to insert them based on the function's arity.
    kj::Maybe<jsg::JsArray> argsArrayFromClient;
    size_t argCountFromClient = 0;
    KJ_IF_SOME(a, args) {
      auto array = KJ_REQUIRE_NONNULL(
          deserializeJsValue(js, a).tryCast<jsg::JsArray>(),
          "expected JsArray when deserializing arguments.");
      argCountFromClient = array.size();
      argsArrayFromClient = kj::mv(array);
    }

    // For now, we are disallowing multiple arguments with bare function syntax, due to a footgun:
    // if you forget to add `env, ctx` to your arg list, then the last arguments from the client
    // will be replaced with `env` and `ctx`. Probably this would be quickly noticed in testing,
    // but if you were to accidentally reflect `env` back to the client, it would be a severe
    // security flaw.
    JSG_REQUIRE(arity == 3, TypeError,
        "Cannot call handler function \"", methodName, "\" over RPC because it has the wrong "
        "number of arguments. A simple function handler can only be called over RPC if it has "
        "exactly the arguments (arg, env, ctx), where only the first argument comes from the "
        "client. To support multi-argument RPC functions, use class-based syntax (extending "
        "WorkerEntrypoint) instead.");
    JSG_REQUIRE(argCountFromClient == 1, TypeError,
        "Attempted to call RPC function \"", methodName, "\" with the wrong number of arguments. "
        "When calling a top-level handler function that is not declared as part of a class, you "
        "must always send exactly one argument. In order to support variable numbers of "
        "arguments, the server must use class-based syntax (extending WorkerEntrypoint) "
        "instead.");

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

  kj::Own<IoContext::WeakRef> weakIoContext;
};

class TransientJsRpcTarget final: public JsRpcTargetBase {
public:
  TransientJsRpcTarget(IoContext& ioCtx, jsg::JsRef<jsg::JsObject> object)
      : JsRpcTargetBase(ioCtx), object(kj::mv(object)) {}

  TargetInfo getTargetInfo(Worker::Lock& lock, IoContext& ioCtx) {
    return {
      .target = object.getHandle(lock),
      .envCtx = kj::none,
    };
  }

private:
  jsg::JsRef<jsg::JsObject> object;
};

jsg::Ref<JsRpcStub> JsRpcStub::constructor(jsg::Lock& js, jsg::Ref<JsRpcTarget> object) {
  auto& ioctx = IoContext::current();

  // We really only took `jsg::Ref<JsRpcTarget>` as the input type for type-checking reasons, but
  // we'd prefer to store the JS handle. There definitely must be one since we just received this
  // object from JS.
  auto handle = jsg::JsRef<jsg::JsObject>(js,
      jsg::JsObject(KJ_ASSERT_NONNULL(object.tryGetHandle(js))));

  rpc::JsRpcTarget::Client cap = kj::heap<TransientJsRpcTarget>(ioctx, kj::mv(handle));

  return jsg::alloc<JsRpcStub>(ioctx.addObject(kj::heap(kj::mv(cap))));
}

void JsRpcTarget::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  // Serialize by effectively creating a `JsRpcStub` around this object and serializing that.
  // Except we don't actually want to do _exactly_ that, because we do not want to actually create
  // a `JsRpcStub` locally. So do the important parts of `JsRpcStub::constructor()` followed by
  // `JsRpcStub::serialize()`.

  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "Remote RPC references can only be serialized for RPC.");
  auto externalHandler = dynamic_cast<RpcSerializerExternalHander*>(&handler);
  JSG_REQUIRE(externalHandler != nullptr, DOMDataCloneError,
      "Remote RPC references can only be serialized for RPC.");

  // Handle can't possibly be missing during serialization, it's how we got here.
  auto handle = jsg::JsRef<jsg::JsObject>(js,
      jsg::JsObject(KJ_ASSERT_NONNULL(JSG_THIS.tryGetHandle(js))));

  rpc::JsRpcTarget::Client cap = kj::heap<TransientJsRpcTarget>(
      IoContext::current(), kj::mv(handle));

  externalHandler->write([cap = kj::mv(cap)](rpc::JsValue::External::Builder builder) mutable {
    builder.setRpcTarget(kj::mv(cap));
  });
}

// JsRpcTarget implementation specific to entrypoints. This is used to deliver the first, top-level
// call of an RPC session.
class EntrypointJsRpcTarget final: public JsRpcTargetBase {
public:
  EntrypointJsRpcTarget(IoContext& ioCtx, kj::Maybe<kj::StringPtr> entrypointName)
      : JsRpcTargetBase(ioCtx),
        // Most of the time we don't really have to clone this but it's hard to fully prove, so
        // let's be safe.
        entrypointName(entrypointName.map([](kj::StringPtr s) { return kj::str(s); })) {}

  TargetInfo getTargetInfo(Worker::Lock& lock, IoContext& ioCtx) {
    jsg::Lock& js = lock;

    auto handler = KJ_REQUIRE_NONNULL(lock.getExportedHandler(entrypointName, ioCtx.getActor()),
                                      "Failed to get handler to worker.");

    if (handler->missingSuperclass) {
      // JS RPC is not enabled on the server side, we cannot call any methods.
      JSG_REQUIRE(FeatureFlags::get(js).getJsRpc(), TypeError,
          "The receiving Durable Object does not support RPC, because its class was not declared "
          "with `extends DurableObject`. In order to enable RPC, make sure your class "
          "extends the special class `DurableObject`, which can be imported from the module "
          "\"cloudflare:workers\".");
    }

    return {
      .target = handler->self.getHandle(lock),
      .envCtx = handler->ctx.map([&](jsg::Ref<ExecutionContext>& execCtx) -> EnvCtx {
        return {
          .env = handler->env.getHandle(js),
          .ctx = lock.getWorker().getIsolate().getApi()
              .wrapExecutionContext(js, execCtx.addRef()),
        };
      })
    };
  }

private:
  kj::Maybe<kj::String> entrypointName;
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
          kj::heap<EntrypointJsRpcTarget>(incomingRequest->getContext(), entrypointName),
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
  // collector to actually collect the JsRpcStub objects.
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
    const v8::FunctionCallbackInfo<v8::Value>& args,
    jsg::Ref<ExecutionContext> ctx, jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* delcare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return jsg::alloc<WorkerEntrypoint>();
}

jsg::Ref<DurableObjectBase> DurableObjectBase::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    jsg::Ref<DurableObjectState> ctx, jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* delcare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return jsg::alloc<DurableObjectBase>();
}

}; // namespace workerd::api
