// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/features.h>
#include <workerd/jsg/ser.h>

#include <capnp/membrane.h>

namespace workerd::api {

namespace {

using StreamSinkFulfiller = kj::Own<kj::PromiseFulfiller<rpc::JsValue::StreamSink::Client>>;

}  // namespace

// Implementation of StreamSink RPC interface. The stream sender calls `startStream()` when
// serializing each stream, and the recipient calls `setSlot()` when deserializing streams to
// provide the appropriate destination capability. This class is designed to allow these two
// calls to happen in either order for each slot.
class StreamSinkImpl final: public rpc::JsValue::StreamSink::Server, public kj::Refcounted {
 public:
  ~StreamSinkImpl() noexcept(false) {
    for (auto& slot: table) {
      KJ_IF_SOME(f, slot.tryGet<StreamFulfiller>()) {
        f->reject(KJ_EXCEPTION(FAILED, "expected startStream() was never received"));
      }
    }
  }

  void setSlot(uint i, capnp::Capability::Client stream) {
    if (table.size() <= i) table.resize(i + 1);

    if (table[i] == nullptr) {
      table[i] = kj::mv(stream);
    } else KJ_SWITCH_ONEOF(table[i]) {
      KJ_CASE_ONEOF(stream, capnp::Capability::Client) {
        KJ_FAIL_REQUIRE("setSlot() tried to set the same slot twice", i);
      }
      KJ_CASE_ONEOF(fulfiller, StreamFulfiller) {
        fulfiller->fulfill(kj::mv(stream));
        table[i] = Consumed();
      }
      KJ_CASE_ONEOF(_, Consumed) {
        KJ_FAIL_REQUIRE("setSlot() tried to set the same slot twice", i);
      }
    }
  }

  kj::Promise<void> startStream(StartStreamContext context) override {
    uint i = context.getParams().getExternalIndex();

    if (table.size() <= i) {
      // guard against ridiculous table allocation
      JSG_REQUIRE(i < 1024, Error, "Too many streams in one message.");
      table.resize(i + 1);
    }

    if (table[i] == nullptr) {
      auto paf = kj::newPromiseAndFulfiller<capnp::Capability::Client>();
      table[i] = kj::mv(paf.fulfiller);
      context.getResults(capnp::MessageSize{4, 1}).setStream(kj::mv(paf.promise));
    } else KJ_SWITCH_ONEOF(table[i]) {
      KJ_CASE_ONEOF(stream, capnp::Capability::Client) {
        context.getResults(capnp::MessageSize{4, 1}).setStream(kj::mv(stream));
        table[i] = Consumed();
      }
      KJ_CASE_ONEOF(fulfiller, StreamFulfiller) {
        KJ_FAIL_REQUIRE("startStream() tried to start the same stream twice", i);
      }
      KJ_CASE_ONEOF(_, Consumed) {
        KJ_FAIL_REQUIRE("startStream() tried to start the same stream twice", i);
      }
    }

    return kj::READY_NOW;
  }

 private:
  using StreamFulfiller = kj::Own<kj::PromiseFulfiller<capnp::Capability::Client>>;
  struct Consumed {};

  // Each slot starts out null (uninitialized). It becomes a Capability::Client if setSlot() is
  // called first, or a StreamFulfiller if startStream() is called first. It becomse `Consumed`
  // when the other method is called.
  // HACK: Slots in the table take advantage of the little-known fact that OneOf has a "null"
  //   value, which is the value a OneOf has when default-initialized. This is useful because we
  //   don't want to explicitly initialize skipped slots. Maybe<OneOf> would be another option
  //   here, but would add 8 bytes to every slot just to store a boolean... feels bloated. There
  //   are only two methods in this class so I think it's OK.
  using Slot = kj::OneOf<capnp::Capability::Client, StreamFulfiller, Consumed>;

  kj::Vector<Slot> table;
};

capnp::Capability::Client RpcSerializerExternalHander::writeStream(BuilderCallback callback) {
  rpc::JsValue::StreamSink::Client* streamSinkPtr;
  KJ_IF_SOME(ss, streamSink) {
    streamSinkPtr = &ss;
  } else {
    // First stream written, set up the StreamSink.
    streamSinkPtr = &streamSink.emplace(getStreamSinkFunc());
  }

  auto result = ({
    auto req = streamSinkPtr->startStreamRequest(capnp::MessageSize{4, 0});
    req.setExternalIndex(externals.size());
    req.send().getStream();
  });

  write(kj::mv(callback));

  return result;
}

capnp::Orphan<capnp::List<rpc::JsValue::External>> RpcSerializerExternalHander::build(
    capnp::Orphanage orphanage) {
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

void RpcDeserializerExternalHander::setLastStream(capnp::Capability::Client stream) {
  KJ_IF_SOME(ss, streamSink) {
    ss.setSlot(i - 1, kj::mv(stream));
  } else {
    auto ss = kj::refcounted<StreamSinkImpl>();
    ss->setSlot(i - 1, kj::mv(stream));
    streamSink = *ss;
    streamSinkCap = rpc::JsValue::StreamSink::Client(kj::mv(ss));
  }
}

namespace {

// Call to construct an `rpc::JsValue` from a JS value.
//
// `makeBuilder` is a function which takes a capnp::MessageSize hint and returns the
// rpc::JsValue::Builder to fill in.
template <typename Func>
void serializeJsValue(jsg::Lock& js,
    jsg::JsValue value,
    Func makeBuilder,
    RpcSerializerExternalHander::GetStreamSinkFunc getStreamSinkFunc) {
  RpcSerializerExternalHander externalHandler(kj::mv(getStreamSinkFunc));

  jsg::Serializer serializer(js,
      jsg::Serializer::Options{
        .version = 15,
        .omitHeader = false,
        .treatClassInstancesAsPlainObjects = false,
        .externalHandler = externalHandler,
      });
  serializer.write(js, value);
  kj::Array<const byte> data = serializer.release().data;
  JSG_ASSERT(data.size() <= MAX_JS_RPC_MESSAGE_SIZE, Error,
      "Serialized RPC arguments or return values are limited to 1MiB, but the size of this value "
      "was: ",
      data.size(), " bytes.");

  capnp::MessageSize hint{0, 0};
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
    builder.adoptExternals(
        externalHandler.build(capnp::Orphanage::getForMessageContaining(builder)));
  }
}

struct DeserializeResult {
  jsg::JsValue value;
  kj::Own<RpcStubDisposalGroup> disposalGroup;
  kj::Maybe<rpc::JsValue::StreamSink::Client> streamSink;
};

// Call to construct a JS value from an `rpc::JsValue`.
DeserializeResult deserializeJsValue(
    jsg::Lock& js, rpc::JsValue::Reader reader, kj::Maybe<StreamSinkImpl&> streamSink = kj::none) {
  auto disposalGroup = kj::heap<RpcStubDisposalGroup>();

  RpcDeserializerExternalHander externalHandler(reader.getExternals(), *disposalGroup, streamSink);

  jsg::Deserializer deserializer(js, reader.getV8Serialized(), kj::none, kj::none,
      jsg::Deserializer::Options{
        .version = 15,
        .readHeader = true,
        .externalHandler = externalHandler,
      });

  return {
    .value = deserializer.readValue(js),
    .disposalGroup = kj::mv(disposalGroup),
    .streamSink = externalHandler.getStreamSink(),
  };
}

// Does deserializeJsValue() and then adds a `dispose()` method to the returned object (if it is
// an object) which disposes all stubs therein.
jsg::JsValue deserializeRpcReturnValue(
    jsg::Lock& js, rpc::JsRpcTarget::CallResults::Reader callResults, StreamSinkImpl& streamSink) {
  auto [value, disposalGroup, _] = deserializeJsValue(js, callResults.getResult(), streamSink);

  // If the object had a disposer on the callee side, it will run when we discard the callPipeline,
  // so attach that to the disposal group on the caller side. If the returned object did NOT have
  // a disposer then we should discard callPipeline so that we don't hold open the callee's
  // context for no reason.
  if (callResults.getHasDisposer()) {
    disposalGroup->setCallPipeline(
        IoContext::current().addObject(kj::heap(callResults.getCallPipeline())));
  }

  KJ_IF_SOME(obj, value.tryCast<jsg::JsObject>()) {
    if (obj.isInstanceOf<JsRpcStub>(js)) {
      // We're returning a plain stub. We don't need to override its `dispoose` method.
      disposalGroup->disownAll();
    } else {
      // Add a dispose method to the return object that disposes the DisposalGroup.
      v8::Local<v8::Value> func = js.wrapSimpleFunction(js.v8Context(),
          [disposalGroup = kj::mv(disposalGroup)](jsg::Lock&,
              const v8::FunctionCallbackInfo<v8::Value>&) mutable { disposalGroup->disposeAll(); });
      obj.setNonEnumerable(js, js.symbolDispose(), jsg::JsValue(func));
    }
  } else {
    // Result wasn't an object, so it must not contain any stubs.
    KJ_ASSERT(disposalGroup->empty());
  }

  return value;
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
  explicit RevokerMembrane(kj::Promise<void> promise): promise(promise.fork()) {}

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

// Given a value, check if it has a dispose method and, if so, invoke it.
void tryCallDisposeMethod(jsg::Lock& js, jsg::JsValue value) {
  js.withinHandleScope([&]() {
    KJ_IF_SOME(obj, value.tryCast<jsg::JsObject>()) {
      auto dispose = obj.get(js, js.symbolDispose());
      if (dispose.isFunction()) {
        jsg::check(v8::Local<v8::Value>(dispose).As<v8::Function>()->Call(
            js.v8Context(), value, 0, nullptr));
      }
    }
  });
}

}  // namespace

JsRpcPromise::JsRpcPromise(jsg::JsRef<jsg::JsPromise> inner,
    kj::Own<WeakRef> weakRefParam,
    IoOwn<rpc::JsRpcTarget::CallResults::Pipeline> pipeline)
    : inner(kj::mv(inner)),
      weakRef(kj::mv(weakRefParam)),
      state(Pending{kj::mv(pipeline)}) {
  KJ_REQUIRE(weakRef->ref == kj::none);
  weakRef->ref = *this;
}
JsRpcPromise::~JsRpcPromise() noexcept(false) {
  weakRef->ref = kj::none;
}

void JsRpcPromise::resolve(jsg::Lock& js, jsg::JsValue result) {
  if (state.is<Pending>()) {
    state = Resolved{
      .result = jsg::Value(js.v8Isolate, result),
      .ctxCheck = IoContext::current().addObject(*this),
    };
  } else {
    // We'd better dispose this.
    tryCallDisposeMethod(js, result);
  }
}

void JsRpcPromise::dispose(jsg::Lock& js) {
  KJ_IF_SOME(resolved, state.tryGet<Resolved>()) {
    // Disposing the promise implies disposing the final result.
    tryCallDisposeMethod(js, jsg::JsValue(resolved.result.getHandle(js)));
  }

  state = Disposed();
  weakRef->disposed = true;
}

// See comment at call site for explanation.
static rpc::JsRpcTarget::Client makeJsRpcTargetForSingleLoopbackCall(
    jsg::Lock& js, jsg::JsObject obj);

rpc::JsRpcTarget::Client JsRpcPromise::getClientForOneCall(
    jsg::Lock& js, kj::Vector<kj::StringPtr>& path) {
  // (Don't extend `path` because we're the root.)

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(pending, Pending) {
      return pending.pipeline->getCallPipeline();
    }
    KJ_CASE_ONEOF(resolved, Resolved) {
      // Dereference `ctxCheck` just to verify we're running in the correct context. (If not,
      // this will throw.)
      *resolved.ctxCheck;

      // A value was already returned, and we closed the original RPC pipeline. But the application
      // kept the promise around and is still trying to pipeline on it. What do we do?
      //
      // A naive answer would be: We just return the actual value that was returned originally.
      // Like if someone asked for `promise.foo.bar`, we just give them `returnValue.foo.bar`.
      //
      // That doesn't quite work, for a couple reasons:
      // * If the caller is awaiting a property, they expect the result will have a `dispose()`
      //   method added to it, and that any stubs in the result will be independently disposable.
      //   This essentially means we need to clone the value so that we can dup() all the stubs and
      //   modify the result.
      // * If the caller is trying to make a pipelined RPC call, they expect this call to go
      //   through all the usual RPC machinery. They do NOT expect that this is going to be a local
      //   call.
      //
      // The easiest way to make this all just work is... to actually wrap the value in a one-off
      // RPC stub, and make a real RPC on it.

      return js.withinHandleScope([&]() -> rpc::JsRpcTarget::Client {
        auto value = jsg::JsValue(resolved.result.getHandle(js));

        KJ_IF_SOME(obj, value.tryCast<jsg::JsObject>()) {
          KJ_IF_SOME(stub, obj.tryUnwrapAs<JsRpcStub>(js)) {
            // Oh, the return value is actually a stub itself. Just use it.
            return stub->getClient();
          } else {
            // Must be a plain object.
            return makeJsRpcTargetForSingleLoopbackCall(js, obj);
          }
        } else {
          JSG_FAIL_REQUIRE(TypeError, "Can't pipeline on RPC that did not return an object.");
        }
      });
    }
    KJ_CASE_ONEOF(disposed, Disposed) {
      return JSG_KJ_EXCEPTION(FAILED, Error, "RPC promise used after being disposed.");
    }
  }
  KJ_UNREACHABLE;
}

rpc::JsRpcTarget::Client JsRpcProperty::getClientForOneCall(
    jsg::Lock& js, kj::Vector<kj::StringPtr>& path) {
  auto result = parent->getClientForOneCall(js, path);
  path.add(name);
  return result;
}

namespace {

struct JsRpcPromiseAndPipleine {
  jsg::JsPromise promise;
  kj::Own<JsRpcPromise::WeakRef> weakRef;
  rpc::JsRpcTarget::CallResults::Pipeline pipeline;

  jsg::Ref<JsRpcPromise> asJsRpcPromise(jsg::Lock& js) && {
    return jsg::alloc<JsRpcPromise>(jsg::JsRef<jsg::JsPromise>(js, promise), kj::mv(weakRef),
        IoContext::current().addObject(kj::heap(kj::mv(pipeline))));
  }
};

// Core implementation of making an RPC call, reusable for many cases below.
JsRpcPromiseAndPipleine callImpl(jsg::Lock& js,
    JsRpcClientProvider& parent,
    kj::Maybe<const kj::String&> name,
    // If `maybeArgs` is provided, this is a call, otherwise it is a property access.
    kj::Maybe<const v8::FunctionCallbackInfo<v8::Value>&> maybeArgs) {
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

  try {
    return js.tryCatch([&]() -> JsRpcPromiseAndPipleine {
      // `path` will be filled in with the path of property names leading from the stub represented by
      // `client` to the specific property / method that we're trying to invoke.
      kj::Vector<kj::StringPtr> path;
      auto client = parent.getClientForOneCall(js, path);

      auto& ioContext = IoContext::current();

      KJ_IF_SOME(lock, ioContext.waitForOutputLocksIfNecessary()) {
        // Replace the client with a promise client that will delay thecall until the output gate
        // is open.
        client = lock.then([client = kj::mv(client)]() mutable { return kj::mv(client); });
      }

      auto builder = client.callRequest();

      // This code here is slightly overcomplicated in order to avoid pushing anything to the
      // kj::Vector in the common case that the parent path is empty. I'm probably trying too hard
      // but oh well.
      if (path.empty()) {
        KJ_IF_SOME(n, name) {
          builder.setMethodName(n);
        } else {
          // No name and no path, must be directly calling a stub.
          builder.initMethodPath(0);
        }
      } else {
        auto pathBuilder = builder.initMethodPath(path.size() + (name != kj::none));
        for (auto i: kj::indices(path)) {
          pathBuilder.set(i, path[i]);
        }
        KJ_IF_SOME(n, name) {
          pathBuilder.set(path.size(), n);
        }
      }

      kj::Maybe<StreamSinkFulfiller> paramsStreamSinkFulfiller;

      KJ_IF_SOME(args, maybeArgs) {
        // If we have arguments, serialize them.
        // Note that we may fail to serialize some element, in which case this will throw back to
        // JS.
        if (args.Length() > 0) {
          // This is a function call with arguments.
          v8::LocalVector<v8::Value> argv(js.v8Isolate, args.Length());
          for (int n = 0; n < args.Length(); n++) {
            argv[n] = args[n];
          }
          auto arr = v8::Array::New(js.v8Isolate, argv.data(), argv.size());

          serializeJsValue(js, jsg::JsValue(arr), [&](capnp::MessageSize hint) {
            // TODO(perf): Actually use the size hint.
            return builder.getOperation().initCallWithArgs();
          }, [&]() -> rpc::JsValue::StreamSink::Client {
            // A stream was encountered in the params, so we must expect the response to contain
            // paramsStreamSink. But we don't have the response yet. So, we need to set up a
            // temporary promise client, which we hook to the response a little bit later.
            auto paf = kj::newPromiseAndFulfiller<rpc::JsValue::StreamSink::Client>();
            paramsStreamSinkFulfiller = kj::mv(paf.fulfiller);
            return kj::mv(paf.promise);
          });
        }
      } else {
        // This is a property access.
        builder.getOperation().setGetProperty();
      }

      // Unfortunately, we always have to send a `resultsStreamSink` because we don't know until
      // after the call completes whether or not it will return any streams. If it's unused,
      // though, it should only be a couple allocations.
      auto resultStreamSink = kj::refcounted<StreamSinkImpl>();
      builder.setResultsStreamSink(kj::addRef(*resultStreamSink));

      auto callResult = builder.send();

      KJ_IF_SOME(ssf, paramsStreamSinkFulfiller) {
        ssf->fulfill(callResult.getParamsStreamSink());
      }

      // We need to arrange that our JsRpcPromise will updated in-place with the final settlement
      // of this RPC promise. However, we can't actually construct the JsRpcPromise until we have
      // the final promise to give it. To resolve the cycle, we only create a JsRpcPromise::WeakRef
      // here, which is filled in later on to point at the JsRpcPromise, if and when one is created.
      auto weakRef = kj::atomicRefcounted<JsRpcPromise::WeakRef>();

      // RemotePromise lets us consume its pipeline and promise portions independently; we consume
      // the promise here and we consume the pipeline below, both via kj::mv().
      auto jsPromise = ioContext.awaitIo(js, kj::mv(callResult),
          [weakRef = kj::atomicAddRef(*weakRef), resultStreamSink = kj::mv(resultStreamSink)](
              jsg::Lock& js,
              capnp::Response<rpc::JsRpcTarget::CallResults> response) mutable -> jsg::Value {
        auto jsResult = deserializeRpcReturnValue(js, response, *resultStreamSink);

        if (weakRef->disposed) {
          // The promise was explicitly disposed before it even resolved. This means we must dispose
          // the returned object as well.
          tryCallDisposeMethod(js, jsResult);
        } else {
          KJ_IF_SOME(r, weakRef->ref) {
            r.resolve(js, jsResult);
          }
        }

        return jsg::Value(js.v8Isolate, jsResult);
      });

      return {
        .promise = jsg::JsPromise(js.wrapSimplePromise(kj::mv(jsPromise))),
        .weakRef = kj::mv(weakRef),
        .pipeline = kj::mv(callResult),
      };
    }, [&](jsg::Value error) -> JsRpcPromiseAndPipleine {
      // Probably a serialization error. Need to convert to an async error since we never throw
      // synchronously from async functions.
      auto jsError = jsg::JsValue(error.getHandle(js));
      auto pipeline = capnp::newBrokenPipeline(js.exceptionToKj(jsError));
      return {.promise = js.rejectedJsPromise(jsError),
        .weakRef = kj::atomicRefcounted<JsRpcPromise::WeakRef>(),
        .pipeline =
            rpc::JsRpcTarget::CallResults::Pipeline(capnp::AnyPointer::Pipeline(kj::mv(pipeline)))};
    });
  } catch (jsg::JsExceptionThrown&) {
    // This must be a termination exception, or we would have caught it above.
    throw;
  } catch (...) {
    // Catch KJ exceptions and make them async, since we don't want async calls to throw
    // synchronously.
    auto e = kj::getCaughtExceptionAsKj();
    auto pipeline = capnp::newBrokenPipeline(kj::cp(e));
    return {
      .promise = jsg::JsPromise(js.wrapSimplePromise(js.rejectedPromise<jsg::Value>(kj::mv(e)))),
      .weakRef = kj::atomicRefcounted<JsRpcPromise::WeakRef>(),
      .pipeline =
          rpc::JsRpcTarget::CallResults::Pipeline(capnp::AnyPointer::Pipeline(kj::mv(pipeline)))};
  }
}

}  // namespace

jsg::Ref<JsRpcPromise> JsRpcProperty::call(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  return callImpl(js, *parent, name, args).asJsRpcPromise(js);
}

jsg::Ref<JsRpcPromise> JsRpcStub::call(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  return callImpl(js, *this, kj::none, args).asJsRpcPromise(js);
}

jsg::Ref<JsRpcPromise> JsRpcPromise::call(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  return callImpl(js, *this, kj::none, args).asJsRpcPromise(js);
}

namespace {

jsg::JsValue thenImpl(jsg::Lock& js,
    v8::Local<v8::Promise> promise,
    v8::Local<v8::Function> handler,
    jsg::Optional<v8::Local<v8::Function>> errorHandler) {
  KJ_IF_SOME(e, errorHandler) {
    // Note that we intentionally propagate any exception from promise->Then() sychronously since
    // if V8's native Promise threw synchronously from `then()`, we might as well too. Anyway it's
    // probably a termination exception.
    return jsg::JsPromise(jsg::check(promise->Then(js.v8Context(), handler, e)));
  } else {
    return jsg::JsPromise(jsg::check(promise->Then(js.v8Context(), handler)));
  }
}

jsg::JsValue catchImpl(
    jsg::Lock& js, v8::Local<v8::Promise> promise, v8::Local<v8::Function> errorHandler) {
  return jsg::JsPromise(jsg::check(promise->Catch(js.v8Context(), errorHandler)));
}

jsg::JsValue finallyImpl(
    jsg::Lock& js, v8::Local<v8::Promise> promise, v8::Local<v8::Function> onFinally) {
  // HACK: `finally()` is not exposed as a C++ API, so we have to manually read it from JS.
  jsg::JsObject obj(promise);
  auto func = obj.get(js, "finally");
  KJ_ASSERT(func.isFunction());
  v8::Local<v8::Value> param = onFinally;
  return jsg::JsValue(jsg::check(
      v8::Local<v8::Value>(func).As<v8::Function>()->Call(js.v8Context(), obj, 1, &param)));
}

}  // namespace

jsg::JsValue JsRpcProperty::then(jsg::Lock& js,
    v8::Local<v8::Function> handler,
    jsg::Optional<v8::Local<v8::Function>> errorHandler) {
  auto promise = callImpl(js, *parent, name, kj::none).promise;

  return thenImpl(js, promise, handler, errorHandler);
}

jsg::JsValue JsRpcProperty::catch_(jsg::Lock& js, v8::Local<v8::Function> errorHandler) {
  auto promise = callImpl(js, *parent, name, kj::none).promise;

  return catchImpl(js, promise, errorHandler);
}

jsg::JsValue JsRpcProperty::finally(jsg::Lock& js, v8::Local<v8::Function> onFinally) {
  auto promise = callImpl(js, *parent, name, kj::none).promise;

  return finallyImpl(js, promise, onFinally);
}

jsg::JsValue JsRpcPromise::then(jsg::Lock& js,
    v8::Local<v8::Function> handler,
    jsg::Optional<v8::Local<v8::Function>> errorHandler) {
  return thenImpl(js, inner.getHandle(js), handler, errorHandler);
}

jsg::JsValue JsRpcPromise::catch_(jsg::Lock& js, v8::Local<v8::Function> errorHandler) {
  return catchImpl(js, inner.getHandle(js), errorHandler);
}

jsg::JsValue JsRpcPromise::finally(jsg::Lock& js, v8::Local<v8::Function> onFinally) {
  return finallyImpl(js, inner.getHandle(js), onFinally);
}

kj::Maybe<jsg::Ref<JsRpcProperty>> JsRpcProperty::getProperty(jsg::Lock& js, kj::String name) {
  return jsg::alloc<JsRpcProperty>(JSG_THIS, kj::mv(name));
}

kj::Maybe<jsg::Ref<JsRpcProperty>> JsRpcPromise::getProperty(jsg::Lock& js, kj::String name) {
  return jsg::alloc<JsRpcProperty>(JSG_THIS, kj::mv(name));
}

JsRpcStub::JsRpcStub(
    IoOwn<rpc::JsRpcTarget::Client> capnpClient, RpcStubDisposalGroup& disposalGroup)
    : capnpClient(kj::mv(capnpClient)),
      disposalGroup(disposalGroup) {
  disposalGroup.list.add(*this);
}

JsRpcStub::~JsRpcStub() noexcept(false) {
  KJ_IF_SOME(d, disposalGroup) {
    d.list.remove(*this);
  }

  KJ_IF_SOME(c, capnpClient) {
    // The app failed to dispose the stub; it leaked. We'd rather not make GC observable, so we
    // must pass the capnp capability off to the I/O context to be dropped when the I/O context
    // itself shuts down.
    kj::mv(c).deferGcToContext();

    // In preview, let's try to warn the developer about the problem.
    //
    // TODO(cleanup): Instead of logging this warning at GC time, it would be better if we logged
    //   it at the time that the client is destroyed, i.e. when the IoContext is torn down,
    //   which is usually sooner (and more deterministic). But logging a warning during
    //   IoContext tear-down is problematic since logWarningOnce() is a method on
    //   IoContext...
    if (IoContext::hasCurrent()) {
      IoContext::current().logWarningOnce(kj::str(
          "An RPC stub was not disposed properly. You must call dispose() on all stubs in order to "
          "let the other side know that you are no longer using them. You cannot rely on "
          "the garbage collector for this because it may take arbitrarily long before actually "
          "collecting unreachable objects. As a shortcut, calling dispose() on the result of "
          "an RPC call disposes all stubs within it."));
    }
  }
}

RpcStubDisposalGroup::~RpcStubDisposalGroup() noexcept(false) {
  if (jsg::isInGcDestructor()) {
    // If the disposal group was dropped as a result of garbage collection, we should NOT actually
    // dispose any stubs. In particular:
    // * If an application never invokes dispose() on an RPC result and the result is GC'd, the
    //   app could still be holding onto stubs that came from that result. We don't want to
    //   dispose those unexpectedly.
    // * If an incoming RPC call does something like `await new Promise(() => {})` to hang
    //   forever, the promise reaction can be GC'd even though the call didn't really complete.
    //   We don't want to dispose param stubs in this case.
    disownAll();

    // If we have a `callPipeline`, it means we called an RPC that returned an object, and that
    // object had a dispose method defined on the server side. We don't want it to observe GC,
    // so we'll defer dropping the pipeline until the IoContext is destroyed.
    //
    // (We don't do this as part of disownAll() because the one other call site of disownAll()
    // is only invoked in cases where there shouldn't be a `callPipeline` anyway...)
    KJ_IF_SOME(c, callPipeline) {
      kj::mv(c).deferGcToContext();

      // In preview, let's try to warn the developer about the problem.
      //
      // TODO(cleanup): Same comment as in ~JsRpcStub().
      if (IoContext::hasCurrent()) {
        IoContext::current().logWarningOnce(kj::str(
            "An RPC result was not disposed properly. One of the RPC calls you made expects you "
            "to call dispose() on the return value, but you didn't do so. You cannot rely on "
            "the garbage collector for this because it may take arbitrarily long before actually "
            "collecting unreachable objects."));
      }
    }
  } else {
    // However, if we're destroying the RpcStubDisposalGroup NOT as a result of GC, this probably
    // means one of:
    // * This is the disposal group for an incoming RPC call, and that call completed. The group
    //   was attached to the completion continuation, which executed, and is now being destroyed.
    //   This is the normal completion case, and we should dispose all the param stubs.
    // * An exception was thrown in the RPC implementation before stubs could be passed to
    //   JavaScript in the first place, resulting in the disposal group being destroyed during
    //   exception unwind. The stubs should be disposed proactively since they were never
    //   received.
    disposeAll();
  }
}

rpc::JsRpcTarget::Client JsRpcStub::getClient() {
  KJ_IF_SOME(c, capnpClient) {
    return *c;
  } else {
    // TODO(soon): Improve the error message to describe why it was disposed.
    return JSG_KJ_EXCEPTION(FAILED, Error, "RPC stub used after being disposed.");
  }
}

rpc::JsRpcTarget::Client JsRpcStub::getClientForOneCall(
    jsg::Lock& js, kj::Vector<kj::StringPtr>& path) {
  // (Don't extend `path` because we're the root.)
  return getClient();
}

jsg::Ref<JsRpcStub> JsRpcStub::dup() {
  return jsg::alloc<JsRpcStub>(IoContext::current().addObject(kj::heap(getClient())));
}

void JsRpcStub::dispose() {
  capnpClient = kj::none;
  KJ_IF_SOME(d, disposalGroup) {
    d.list.remove(*this);
    disposalGroup = kj::none;
  }
}

void RpcStubDisposalGroup::disownAll() {
  for (auto& stub: list) {
    stub.disposalGroup = kj::none;
    list.remove(stub);
  }
}

void RpcStubDisposalGroup::disposeAll() {
  for (auto& stub: list) {
    stub.dispose();
  }
  callPipeline = kj::none;

  // Each stub should have removed itself.
  KJ_ASSERT(list.empty());
}

kj::Maybe<jsg::Ref<JsRpcProperty>> JsRpcStub::getRpcMethod(jsg::Lock& js, kj::String name) {
  // Do not return a method for `then`, otherwise JavaScript decides this is a thenable, i.e. a
  // custom Promise, which will mean a Promise that resolves to this object will attempt to chain
  // with it, which is not what you want!
  if (name == "then"_kj) return kj::none;

  return jsg::alloc<JsRpcProperty>(JSG_THIS, kj::mv(name));
}

void JsRpcStub::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "Remote RPC references can only be serialized for RPC.");
  auto externalHandler = dynamic_cast<RpcSerializerExternalHander*>(&handler);
  JSG_REQUIRE(externalHandler != nullptr, DOMDataCloneError,
      "Remote RPC references can only be serialized for RPC.");

  externalHandler->write([cap = getClient()](rpc::JsValue::External::Builder builder) mutable {
    builder.setRpcTarget(kj::mv(cap));
  });

  // Sending a stub over RPC implicitly disposes the stub. The application can explicitly .dup() it
  // if this is undesired.
  dispose();
}

jsg::Ref<JsRpcStub> JsRpcStub::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  auto& handler = KJ_REQUIRE_NONNULL(
      deserializer.getExternalHandler(), "got JsRpcStub on non-RPC serialized object?");
  auto externalHandler = dynamic_cast<RpcDeserializerExternalHander*>(&handler);
  KJ_REQUIRE(externalHandler != nullptr, "got JsRpcStub on non-RPC serialized object?");

  auto reader = externalHandler->read();
  KJ_REQUIRE(reader.isRpcTarget(), "external table slot type doesn't match serialization tag");

  auto& ioctx = IoContext::current();
  return jsg::alloc<JsRpcStub>(
      ioctx.addObject(kj::heap(reader.getRpcTarget())), externalHandler->getDisposalGroup());
}

static bool isFunctionForRpc(jsg::Lock& js, v8::Local<v8::Function> func) {
  jsg::JsObject obj(func);
  if (obj.isInstanceOf<JsRpcProperty>(js) || obj.isInstanceOf<JsRpcPromise>(js)) {
    // Don't allow JsRpcProperty or JsRpcPromise to be treated as plain functions, even though they
    // are technically callable. These types need to be treated specially (if we decide to let
    // them be passed over RPC at all).
    return false;
  }
  return true;
}

static bool isFunctionForRpc(jsg::Lock& js, jsg::JsValue value) {
  if (!value.isFunction()) return false;
  return isFunctionForRpc(js, v8::Local<v8::Value>(value).As<v8::Function>());
}

static inline bool isFunctionForRpc(jsg::Lock& js, v8::Local<v8::Value> val) {
  return isFunctionForRpc(js, jsg::JsValue(val));
}
static inline bool isFunctionForRpc(jsg::Lock& js, jsg::JsObject val) {
  return isFunctionForRpc(js, jsg::JsValue(val));
}

// `makeCallPipeline()` has a bit of a complicated result type..
namespace MakeCallPipeline {
// The value is an object, which may have stubs inside it.
struct Object {
  rpc::JsRpcTarget::Client cap;

  // Was the value a plain JavaScript object which had a custom dispose() method?
  bool hasDispose;
};

// The value was something that should serialize to a single stub (e.g. it was an RpcTarget, a
// plain function, or already a stub). The callPipeline should simply be a copy of that stub.
struct SingleStub {};

// The value is not a type that supports pipelining. It may still be serializable, and it could
// even contain stubs (e.g. in a Map).
struct NonPipelinable {
  // callPipeline to return just for error-handling purposes.
  rpc::JsRpcTarget::Client errorPipeline;
};

using Result = kj::OneOf<Object, SingleStub, NonPipelinable>;
};  // namespace MakeCallPipeline

// Create the callPipeline for a call result.
//
// Defined later in this file.
static MakeCallPipeline::Result makeCallPipeline(jsg::Lock& js, jsg::JsValue value);

// Callee-side implementation of JsRpcTarget.
//
// Most of the implementation is in this base class. There are subclasses specializing for the case
// of a top-level entrypoint vs. a transient object introduced by a previous RPC in the same
// session.
class JsRpcTargetBase: public rpc::JsRpcTarget::Server {
 public:
  JsRpcTargetBase(IoContext& ctx): weakIoContext(ctx.getWeakRef()) {}

  struct EnvCtx {
    v8::Local<v8::Value> env;
    jsg::JsObject ctx;
  };

  struct TargetInfo {
    // The object on which the RPC method should be invoked.
    jsg::JsObject target;

    // If `env` and `ctx` need to be delivered as arguments to the method, these are the values
    // to deliver.
    kj::Maybe<EnvCtx> envCtx;

    bool allowInstanceProperties;
  };

  // Get the object on which the method is to be invoked. This is virtual so that we can have
  // separate subclasses handling the case of an entrypoint vs. a transient RPC object.
  virtual TargetInfo getTargetInfo(Worker::Lock& lock, IoContext& ioCtx) = 0;

  // Handles the delivery of JS RPC method calls.
  kj::Promise<void> call(CallContext callContext) override {
    IoContext& ctx = JSG_REQUIRE_NONNULL(
        weakIoContext->tryGet(), Error, "The destination object for this RPC no longer exists.");

    ctx.getLimitEnforcer().topUpActor();

    // HACK: Cap'n Proto call contexts are documented as being pointer-like types where the backing
    // object's lifetime is that of the RPC call, but in reality they are refcounted under the
    // hood. Since well be executing the call in the JS microtask queue, we have no ability to
    // actually cancel execution if a cancellation arrives over RPC, and at the end of that
    // execution we're going to access the call context to write the results. We could invent some
    // complicated way to skip initializing results in the case the call has been canceled, but
    // it's easier and safer to just grab a refcount on the call context object itself, which
    // fully protects us. So... do that.
    auto ownCallContext = capnp::CallContextHook::from(callContext).addRef();

    // Try to execute the requested method.
    auto promise =
        ctx.run([this, &ctx, callContext, ownCallContext = kj::mv(ownCallContext),
                    ownThis = thisCap()](Worker::Lock& lock) mutable -> kj::Promise<void> {
      jsg::Lock& js = lock;

      auto targetInfo = getTargetInfo(lock, ctx);

      auto params = callContext.getParams();

      // We will try to get the function, if we can't we'll throw an error to the client.
      auto [propHandle, thisArg, methodNameForTrace] =
          tryGetProperty(lock, targetInfo.target, params, targetInfo.allowInstanceProperties);

      addTrace(js, ctx, methodNameForTrace);

      auto op = params.getOperation();

      auto handleResult = [&](InvocationResult&& invocationResult) {
        // Given a handle for the result, if it's a promise, await the promise, then serialize the
        // final result for return.

        kj::Maybe<kj::Own<kj::PromiseFulfiller<rpc::JsRpcTarget::Client>>> callPipelineFulfiller;

        // We need another ref to this fulfiller for the error callback. It can rely on being
        // destroyed at the same time as the success callback.
        kj::Maybe<kj::PromiseFulfiller<rpc::JsRpcTarget::Client>&> callPipelineFulfillerRef;

        KJ_IF_SOME(ss, invocationResult.streamSink) {
          // Since we have a StreamSink, it's important that we hook up the pipeline for that
          // immediately. Annoyingly, that also means we need to hook up a pipeline for
          // callPipeline, which we don't actually have yet, so we need to promise-ify it.

          auto paf = kj::newPromiseAndFulfiller<rpc::JsRpcTarget::Client>();
          callPipelineFulfillerRef = *paf.fulfiller;
          callPipelineFulfiller = kj::mv(paf.fulfiller);

          capnp::PipelineBuilder<rpc::JsRpcTarget::CallResults> builder(16);
          builder.setCallPipeline(kj::mv(paf.promise));
          builder.setParamsStreamSink(ss);
          callContext.setPipeline(builder.build());
        }

        auto result = ctx.awaitJs(js,
            js.toPromise(invocationResult.returnValue)
                .then(js,
                    ctx.addFunctor(
                        [callContext, ownCallContext = kj::mv(ownCallContext),
                            paramDisposalGroup = kj::mv(invocationResult.paramDisposalGroup),
                            paramsStreamSink = kj::mv(invocationResult.streamSink),
                            resultStreamSink = params.getResultsStreamSink(),
                            callPipelineFulfiller = kj::mv(callPipelineFulfiller)](
                            jsg::Lock& js, jsg::Value value) mutable {
          jsg::JsValue resultValue(value.getHandle(js));

          // Call makeCallPipeline before serializing becaues it may need to extract the disposer.
          auto maybePipeline = makeCallPipeline(js, resultValue);

          rpc::JsRpcTarget::CallResults::Builder results = nullptr;
          serializeJsValue(js, resultValue, [&](capnp::MessageSize hint) {
            hint.wordCount += capnp::sizeInWords<rpc::JsRpcTarget::CallResults>();
            hint.capCount += 1;  // for callPipeline
            results = callContext.initResults(hint);
            return results.initResult();
          }, [&]() -> rpc::JsValue::StreamSink::Client {
            // The results contain streams. We return the resultsStreamSink passed in the request.
            return kj::mv(resultStreamSink);
          });

          KJ_SWITCH_ONEOF(maybePipeline) {
            KJ_CASE_ONEOF(obj, MakeCallPipeline::Object) {
              results.setCallPipeline(kj::mv(obj.cap));
              results.setHasDisposer(obj.hasDispose);
            }
            KJ_CASE_ONEOF(obj, MakeCallPipeline::SingleStub) {
              // Serialization should have produced a single stub. We can use that same stub as
              // the callPipeline.
              auto externals = results.asReader().getResult().getExternals();
              KJ_ASSERT(externals.size() == 1);
              auto external = externals[0];
              KJ_ASSERT(external.isRpcTarget());
              results.setCallPipeline(external.getRpcTarget());
            }
            KJ_CASE_ONEOF(nonPipelinable, MakeCallPipeline::NonPipelinable) {
              results.setCallPipeline(kj::mv(nonPipelinable.errorPipeline));
              // leave hasDisposer false
            }
          }

          KJ_IF_SOME(cpf, callPipelineFulfiller) {
            cpf->fulfill(results.getCallPipeline());
          }

          KJ_IF_SOME(ss, paramsStreamSink) {
            results.setParamsStreamSink(kj::mv(ss));
          }

          // paramDisposalGroup will be destroyed when we return (or when this lambda is destroyed
          // as a result of the promise being rejected). This will implicitly dispose the param
          // stubs.
        }),
                    ctx.addFunctor([callPipelineFulfillerRef](jsg::Lock& js, jsg::Value&& error) {
          // If we set up a `callPipeline` early, we have to make sure it propagates the error.
          // (Otherwise we get a PromiseFulfiller error instead, which is pretty useless...)
          KJ_IF_SOME(cpf, callPipelineFulfillerRef) {
            cpf.reject(js.exceptionToKj(error.addRef(js)));
          }
          js.throwException(kj::mv(error));
        })));

        if (ctx.hasOutputGate()) {
          return result.then([weakIoContext = weakIoContext->addRef()]() mutable {
            return KJ_REQUIRE_NONNULL(weakIoContext->tryGet()).waitForOutputLocks();
          });
        } else {
          return result;
        }
      };

      switch (op.which()) {
        case rpc::JsRpcTarget::CallParams::Operation::CALL_WITH_ARGS: {
          JSG_REQUIRE(isFunctionForRpc(js, propHandle), TypeError,
              kj::str("\"", methodNameForTrace, "\" is not a function."));
          auto fn = propHandle.As<v8::Function>();

          kj::Maybe<rpc::JsValue::Reader> args;
          if (op.hasCallWithArgs()) {
            args = op.getCallWithArgs();
          }

          InvocationResult invocationResult;
          KJ_IF_SOME(envCtx, targetInfo.envCtx) {
            invocationResult = invokeFnInsertingEnvCtx(
                js, methodNameForTrace, fn, thisArg, args, envCtx.env, envCtx.ctx);
          } else {
            invocationResult = invokeFn(js, fn, thisArg, args);
          }

          // We have a function, so let's call it and serialize the result for RPC.
          // If the function returns a promise we will wait for the promise to finish so we can
          // serialize the result.
          return handleResult(kj::mv(invocationResult));
        }

        case rpc::JsRpcTarget::CallParams::Operation::GET_PROPERTY:
          return handleResult({.returnValue = propHandle});
      }

      KJ_FAIL_ASSERT("unknown JsRpcTarget::CallParams::Operation", (uint)op.which());
    }).catch_([](kj::Exception&& e) {
      if (jsg::isTunneledException(e.getDescription())) {
        // Annotate exceptions in RPC worker calls as remote exceptions.
        auto description = jsg::stripRemoteExceptionPrefix(e.getDescription());
        if (!description.startsWith("remote.")) {
          // If we already were annotated as remote from some other worker entrypoint, no point
          // adding an additional prefix.
          e.setDescription(kj::str("remote.", description));
        }
      }
      kj::throwFatalException(kj::mv(e));
    });

    // We need to make sure this RPC is canceled if the IoContext is destroyed. To accomplish that,
    // we add the promise as a task on the context itself, and use a separate promise fulfiller to
    // wait on the result.
    auto paf = kj::newPromiseAndFulfiller<void>();
    promise = promise.then([&fulfiller = *paf.fulfiller]() { fulfiller.fulfill(); },
        [&fulfiller = *paf.fulfiller](kj::Exception&& e) { fulfiller.reject(kj::mv(e)); });
    promise = promise.attach(kj::defer([fulfiller = kj::mv(paf.fulfiller)]() mutable {
      if (fulfiller->isWaiting()) {
        fulfiller->reject(JSG_KJ_EXCEPTION(FAILED, Error,
            "The destination execution context for this RPC was canceled while the "
            "call was still running."));
      }
    }));
    ctx.addTask(kj::mv(promise));

    return kj::mv(paf.promise);
  }

  KJ_DISALLOW_COPY_AND_MOVE(JsRpcTargetBase);

 protected:
  kj::Own<IoContext::WeakRef> weakIoContext;

 private:
  // Returns true if the given name cannot be used as a method on this type.
  virtual bool isReservedName(kj::StringPtr name) = 0;

  // Hook for recording trace information.
  virtual void addTrace(jsg::Lock& js, IoContext& ioctx, kj::StringPtr methodName) = 0;

  struct GetPropResult {
    v8::Local<v8::Value> handle;
    v8::Local<v8::Object> thisArg;

    // Method name suitable for use in trace and error messages. May be a pointer into the RPC
    // params reader.
    kj::ConstString methodNameForTrace;
  };

  [[noreturn]] static void failLookup(kj::StringPtr kjName) {
    JSG_FAIL_REQUIRE(
        TypeError, kj::str("The RPC receiver does not implement the method \"", kjName, "\"."));
  }

  GetPropResult tryGetProperty(jsg::Lock& js,
      jsg::JsObject object,
      rpc::JsRpcTarget::CallParams::Reader callParams,
      bool allowInstanceProperties) {
    auto prototypeOfObject = KJ_ASSERT_NONNULL(js.obj().getPrototype(js).tryCast<jsg::JsObject>());

    // Get the named property of `object`.
    auto getProperty = [&](kj::StringPtr kjName) {
      JSG_REQUIRE(!isReservedName(kjName), TypeError,
          kj::str("'", kjName, "' is a reserved method and cannot be called over RPC."));

      jsg::JsValue jsName = js.strIntern(kjName);

      if (allowInstanceProperties) {
        // This is a simple object. Its own properties are considered to be accessible over RPC, but
        // inherited properties (i.e. from Object.prototype) are not.
        if (!object.has(js, jsName, jsg::JsObject::HasOption::OWN)) {
          failLookup(kjName);
        }
        return object.get(js, jsName);
      } else {
        // This is an instance of a valid RPC target class.
        if (object.has(js, jsName, jsg::JsObject::HasOption::OWN)) {
          // We do NOT allow own properties, only class properties.
          failLookup(kjName);
        }

        auto value = object.get(js, jsName);
        if (value == prototypeOfObject.get(js, jsName)) {
          // This property is inherited from the prototype of `Object`. Don't allow.
          failLookup(kjName);
        }

        return value;
      }
    };

    kj::Maybe<jsg::JsValue> result;
    kj::ConstString methodNameForTrace;

    switch (callParams.which()) {
      case rpc::JsRpcTarget::CallParams::METHOD_NAME: {
        kj::StringPtr methodName = callParams.getMethodName();
        result = getProperty(methodName);
        methodNameForTrace = methodName.attach();
        break;
      }

      case rpc::JsRpcTarget::CallParams::METHOD_PATH: {
        auto path = callParams.getMethodPath();
        auto n = path.size();

        if (n == 0) {
          // Call the target itself as a function.
          result = object;
          methodNameForTrace = "(this)"_kjc;
        } else {
          for (auto i: kj::zeroTo(n - 1)) {
            // For each property name except the last, look up the proprety and replace `object`
            // with it.
            kj::StringPtr name = path[i];
            auto next = getProperty(name);

            KJ_IF_SOME(o, next.tryCast<jsg::JsObject>()) {
              object = o;
            } else {
              // Not an object, doesn't have further properties.
              failLookup(name);
            }

            // Decide whether the new object is a suitable RPC target.
            if (object.getPrototype(js) == prototypeOfObject) {
              // Yes. It's a simple object.
              allowInstanceProperties = true;
            } else if (object.isInstanceOf<JsRpcTarget>(js)) {
              // Yes. It's a JsRpcTarget.
              allowInstanceProperties = false;
            } else if (isFunctionForRpc(js, object)) {
              // Yes. It's a function.
              allowInstanceProperties = true;
            } else {
              failLookup(name);
            }
          }

          result = getProperty(path[n - 1]);
          methodNameForTrace = kj::ConstString(kj::strArray(path, "."));
        }

        break;
      }
    }

    return {
      .handle = KJ_ASSERT_NONNULL(result, "unknown CallParams type", (uint)callParams.which()),
      .thisArg = object,
      .methodNameForTrace = kj::mv(methodNameForTrace),
    };
  }

  struct InvocationResult {
    v8::Local<v8::Value> returnValue;
    kj::Maybe<kj::Own<RpcStubDisposalGroup>> paramDisposalGroup;
    kj::Maybe<rpc::JsValue::StreamSink::Client> streamSink;
  };

  // Deserializes the arguments and passes them to the given function.
  static InvocationResult invokeFn(jsg::Lock& js,
      v8::Local<v8::Function> fn,
      v8::Local<v8::Object> thisArg,
      kj::Maybe<rpc::JsValue::Reader> args) {
    // We received arguments from the client, deserialize them back to JS.
    KJ_IF_SOME(a, args) {
      auto [value, disposalGroup, streamSink] = deserializeJsValue(js, a);
      auto args = KJ_REQUIRE_NONNULL(
          value.tryCast<jsg::JsArray>(), "expected JsArray when deserializing arguments.");
      // Call() expects a `Local<Value> []`... so we populate an array.

      v8::LocalVector<v8::Value> arguments(js.v8Isolate, args.size());
      for (size_t i = 0; i < args.size(); ++i) {
        arguments[i] = args.get(js, i);
      }

      InvocationResult result{
        .returnValue =
            jsg::check(fn->Call(js.v8Context(), thisArg, arguments.size(), arguments.data())),
        .streamSink = kj::mv(streamSink),
      };
      if (!disposalGroup->empty()) {
        result.paramDisposalGroup = kj::mv(disposalGroup);
      }
      return result;
    } else {
      return {.returnValue = jsg::check(fn->Call(js.v8Context(), thisArg, 0, nullptr))};
    }
  };

  // Like `invokeFn`, but inject the `env` and `ctx` values between the first and second
  // parameters. Used for service bindings that use functional syntax.
  static InvocationResult invokeFnInsertingEnvCtx(jsg::Lock& js,
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

    kj::Maybe<kj::Own<RpcStubDisposalGroup>> paramDisposalGroup;
    kj::Maybe<rpc::JsValue::StreamSink::Client> streamSink;

    // We're going to pass all the arguments from the client to the function, but we are going to
    // insert `env` and `ctx`. We assume the last two arguments that the function declared are
    // `env` and `ctx`, so we can determine where to insert them based on the function's arity.
    kj::Maybe<jsg::JsArray> argsArrayFromClient;
    size_t argCountFromClient = 0;
    KJ_IF_SOME(a, args) {
      auto [value, disposalGroup, ss] = deserializeJsValue(js, a);
      streamSink = kj::mv(ss);

      auto array = KJ_REQUIRE_NONNULL(
          value.tryCast<jsg::JsArray>(), "expected JsArray when deserializing arguments.");
      argCountFromClient = array.size();
      argsArrayFromClient = kj::mv(array);

      if (!disposalGroup->empty()) {
        paramDisposalGroup = kj::mv(disposalGroup);
      }
    }

    // For now, we are disallowing multiple arguments with bare function syntax, due to a footgun:
    // if you forget to add `env, ctx` to your arg list, then the last arguments from the client
    // will be replaced with `env` and `ctx`. Probably this would be quickly noticed in testing,
    // but if you were to accidentally reflect `env` back to the client, it would be a severe
    // security flaw.
    JSG_REQUIRE(arity == 3, TypeError, "Cannot call handler function \"", methodName,
        "\" over RPC because it has the wrong "
        "number of arguments. A simple function handler can only be called over RPC if it has "
        "exactly the arguments (arg, env, ctx), where only the first argument comes from the "
        "client. To support multi-argument RPC functions, use class-based syntax (extending "
        "WorkerEntrypoint) instead.");
    JSG_REQUIRE(argCountFromClient == 1, TypeError, "Attempted to call RPC function \"", methodName,
        "\" with the wrong number of arguments. "
        "When calling a top-level handler function that is not declared as part of a class, you "
        "must always send exactly one argument. In order to support variable numbers of "
        "arguments, the server must use class-based syntax (extending WorkerEntrypoint) "
        "instead.");

    v8::LocalVector<v8::Value> arguments(js.v8Isolate, kj::max(argCountFromClient + 2, arity));

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

    return {
      .returnValue =
          jsg::check(fn->Call(js.v8Context(), thisArg, arguments.size(), arguments.data())),
      .paramDisposalGroup = kj::mv(paramDisposalGroup),
      .streamSink = kj::mv(streamSink),
    };
  };
};

class TransientJsRpcTarget final: public JsRpcTargetBase {
 public:
  TransientJsRpcTarget(
      jsg::Lock& js, IoContext& ioCtx, jsg::JsObject object, bool allowInstanceProperties = false)
      : JsRpcTargetBase(ioCtx),
        handles(ioCtx.addObjectReverse(kj::heap<Handles>(js, object, kj::none))),
        allowInstanceProperties(allowInstanceProperties),
        pendingEvent(ioCtx.registerPendingEvent()) {
    // Check for the existence of a dispose function now so that the destructor doesn't have to
    // take an isolate lock if there isn't one.
    auto getResult = object.get(js, js.symbolDispose());
    if (getResult.isFunction()) {
      handles->dispose.emplace(js.v8Isolate, v8::Local<v8::Value>(getResult).As<v8::Function>());
    }
  }

  // Use this version of the constructor to pass the dispose function separately.
  TransientJsRpcTarget(jsg::Lock& js,
      IoContext& ioCtx,
      jsg::JsObject object,
      kj::Maybe<v8::Local<v8::Function>> dispose,
      bool allowInstanceProperties = false)
      : JsRpcTargetBase(ioCtx),
        handles(ioCtx.addObjectReverse(kj::heap<Handles>(js, object, dispose))),
        allowInstanceProperties(allowInstanceProperties),
        pendingEvent(ioCtx.registerPendingEvent()) {}

  ~TransientJsRpcTarget() noexcept(false) {
    // If we have a disposer, and the I/O context is not already destroyed, arrange to call the
    // disposer.
    KJ_IF_SOME(ctx, weakIoContext->tryGet()) {
      KJ_IF_SOME(d, handles->dispose) {
        ctx.addTask(ctx.run(
            [dispose = kj::mv(d), object = kj::mv(handles->object)](Worker::Lock& lock) mutable {
          jsg::Lock& js = lock;
          jsg::check(dispose.getHandle(js)->Call(js.v8Context(), object.getHandle(js), 0, nullptr));
        }));
      }
    }
  }

  TargetInfo getTargetInfo(Worker::Lock& lock, IoContext& ioCtx) override {
    return {
      .target = handles->object.getHandle(lock),
      .envCtx = kj::none,
      .allowInstanceProperties = allowInstanceProperties,
    };
  }

 private:
  struct Handles {
    jsg::JsRef<jsg::JsObject> object;
    kj::Maybe<jsg::V8Ref<v8::Function>> dispose;

    Handles(jsg::Lock& js, jsg::JsObject object, kj::Maybe<v8::Local<v8::Function>> dispose)
        : object(js, object),
          dispose(dispose.map([&](v8::Local<v8::Function> func) {
            return jsg::V8Ref<v8::Function>(js.v8Isolate, func);
          })) {}
  };

  // This object could outlive the IoContext (that's why `JsRpcTargetBase` holds a `WeakRef` to the
  // context). That means hypothetically it could also outlive the isolate. We therefore need to
  // place these handles in a `ReverseIoOwn` so that if the `IoContext` dies before we do, they are
  // dropped at that point.
  ReverseIoOwn<Handles> handles;

  bool allowInstanceProperties;

  // An RpcTarget could receive a new call (in the existing IoContext) at any time, therefore
  // its existence counts as a PendingEvent. If we don't hold a PendingEvent, then the IoContext
  // may decide that there's nothing more than can possibly happen in this context, and cancel
  // itself.
  //
  // Note that it's OK if we hold this past the lifetime of the IoContext itself; the PendingEvent
  // becomes detached in that case and has no effect.
  kj::Own<void> pendingEvent;

  bool isReservedName(kj::StringPtr name) override {
    if (  // dup() is reserved to duplicate the stub itself, pointing to the same object.
        name == "dup" ||

        // All JS classes define a method `constructor` on the prototype, but we don't actually
        // want this to be callable over RPC!
        name == "constructor") {
      return true;
    }
    return false;
  }

  void addTrace(jsg::Lock& js, IoContext& ioctx, kj::StringPtr methodName) override {
    // TODO(someday): Trace non-top-level calls?
  }
};

// See comment at call site for explanation.
static rpc::JsRpcTarget::Client makeJsRpcTargetForSingleLoopbackCall(
    jsg::Lock& js, jsg::JsObject obj) {
  // We intentionally do not want to hook up the disposer here since we're not taking ownership
  // of the object.
  return rpc::JsRpcTarget::Client(
      kj::heap<TransientJsRpcTarget>(js, IoContext::current(), obj, kj::none, true));
}

static MakeCallPipeline::Result makeCallPipeline(jsg::Lock& js, jsg::JsValue value) {
  return js.withinHandleScope([&]() -> MakeCallPipeline::Result {
    jsg::JsObject obj = KJ_UNWRAP_OR(value.tryCast<jsg::JsObject>(), {
      // Primitive value. Return a fake pipeline just so that we get nice errors if someone tries
      // to pipeline on it. (If we return null, we'll get "called null capability" out of
      // Cap'n Proto, which will be treated as an internal error.)
      return (MakeCallPipeline::NonPipelinable{
        .errorPipeline = rpc::JsRpcTarget::Client(
            kj::heap<TransientJsRpcTarget>(js, IoContext::current(), js.obj(), kj::none, true))});
    });

    if (obj.getPrototype(js) == js.obj().getPrototype(js)) {
      // It's a plain object.
      kj::Maybe<v8::Local<v8::Function>> maybeDispose;
      jsg::JsValue disposeProperty = obj.get(js, js.symbolDispose());
      if (disposeProperty.isFunction()) {
        maybeDispose = v8::Local<v8::Value>(disposeProperty).As<v8::Function>();
      }

      // We don't want the disposer to be serialized, so delete it from the object. (Remember
      // that a new `dispose()` method will always be added on the client side).
      obj.delete_(js, js.symbolDispose());

      return MakeCallPipeline::Object{
        .cap = rpc::JsRpcTarget::Client(
            kj::heap<TransientJsRpcTarget>(js, IoContext::current(), obj, maybeDispose, true)),
        .hasDispose = maybeDispose != kj::none};
    } else if (obj.isInstanceOf<JsRpcStub>(js)) {
      // It's just a stub. It'll serialize as a single stub, obviously.
      return MakeCallPipeline::SingleStub();
    } else if (obj.isInstanceOf<JsRpcTarget>(js)) {
      // It's an RPC target. It will be serialized as a single stub.
      return MakeCallPipeline::SingleStub();
    } else if (isFunctionForRpc(js, obj)) {
      // It's a plain function. It will be serialized as a single stub.
      return MakeCallPipeline::SingleStub();
    } else {
      // Not an RPC object. Could be a String or other serializable types that derive from Object.
      // Similar to primitive types, we return a fake pipeline for error-handling reasons.
      // TODO(soon): What if someone returns e.g. a Map with a disposer on it? Should we honor that
      //   disposer?
      return MakeCallPipeline::NonPipelinable{
        .errorPipeline = rpc::JsRpcTarget::Client(
            kj::heap<TransientJsRpcTarget>(js, IoContext::current(), js.obj(), kj::none, true))};
    }
  });
}

jsg::Ref<JsRpcStub> JsRpcStub::constructor(jsg::Lock& js, jsg::Ref<JsRpcTarget> object) {
  auto& ioctx = IoContext::current();

  // We really only took `jsg::Ref<JsRpcTarget>` as the input type for type-checking reasons, but
  // we'd prefer to store the JS handle. There definitely must be one since we just received this
  // object from JS.
  auto handle = jsg::JsObject(KJ_ASSERT_NONNULL(object.tryGetHandle(js)));

  rpc::JsRpcTarget::Client cap = kj::heap<TransientJsRpcTarget>(js, ioctx, handle);

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
  auto handle = jsg::JsObject(KJ_ASSERT_NONNULL(JSG_THIS.tryGetHandle(js)));

  rpc::JsRpcTarget::Client cap = kj::heap<TransientJsRpcTarget>(js, IoContext::current(), handle);

  externalHandler->write([cap = kj::mv(cap)](rpc::JsValue::External::Builder builder) mutable {
    builder.setRpcTarget(kj::mv(cap));
  });
}

void RpcSerializerExternalHander::serializeFunction(
    jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Function> func) {
  serializer.writeRawUint32(static_cast<uint>(rpc::SerializationTag::JS_RPC_STUB));

  rpc::JsRpcTarget::Client cap =
      kj::heap<TransientJsRpcTarget>(js, IoContext::current(), jsg::JsObject(func), true);
  write([cap = kj::mv(cap)](rpc::JsValue::External::Builder builder) mutable {
    builder.setRpcTarget(kj::mv(cap));
  });
}

void RpcSerializerExternalHander::serializeProxy(
    jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Proxy> proxy) {
  js.withinHandleScope([&]() {
    auto handle = jsg::JsObject(proxy);

    // Proxies are only allowed to wrap objects that would normally be serialized by writing a
    // stub, e.g. plain objects and RpcTargets. In such cases, we can write a stub pointing to the
    // proxy.
    //
    // However, note that we don't actually want to test the Proxy's *target* directly, because
    // it's possible the Proxy is trying to disguise the target as something else. Instead, we must
    // determine the type by following the prototype chain. That way, if the Proxy overrides
    // getPrototype(), we will honor that override.
    //
    // Note that we don't support functions. This is because our isFunctionForRpc() check is not
    // prototype-based, and as such it's unclear how exactly we should go about checking for a
    // function here. Luckily, you really don't need to use a `Proxy` to wrap a function... you
    // can just use a function.

    // TODO(perf): We should really cache `prototypeOfObject` somewhere so we don't have to create
    //   an object to get it. (We do this other places in this file, too...)
    auto prototypeOfObject = KJ_ASSERT_NONNULL(js.obj().getPrototype(js).tryCast<jsg::JsObject>());
    auto prototypeOfRpcTarget = js.getPrototypeFor<JsRpcTarget>();
    bool allowInstanceProperties = false;
    auto proto = handle.getPrototype(js);
    if (proto == prototypeOfObject) {
      // A regular object. Allow access to instance properties.
      allowInstanceProperties = true;
    } else {
      // Walk the prototype chain looking for RpcTarget.
      for (;;) {
        if (proto == prototypeOfRpcTarget) {
          // An RpcTarget, don't allow instance properties.
          allowInstanceProperties = false;
          break;
        }

        KJ_IF_SOME(protoObj, proto.tryCast<jsg::JsObject>()) {
          proto = protoObj.getPrototype(js);
        } else {
          // End of prototype chain, and didn't find RpcTarget.
          JSG_FAIL_REQUIRE(DOMDataCloneError,
              "Proxy could not be serialized because it is not a valid RPC receiver type. The "
              "Proxy must emulate either a plain object or an RpcTarget, as indicated by the "
              "Proxy's prototype chain.");
        }
      }
    }

    // Great, we've concluded we can indeed point a stub at this proxy.
    serializer.writeRawUint32(static_cast<uint>(rpc::SerializationTag::JS_RPC_STUB));

    rpc::JsRpcTarget::Client cap =
        kj::heap<TransientJsRpcTarget>(js, IoContext::current(), handle, allowInstanceProperties);
    write([cap = kj::mv(cap)](rpc::JsValue::External::Builder builder) mutable {
      builder.setRpcTarget(kj::mv(cap));
    });
  });
}

// JsRpcTarget implementation specific to entrypoints. This is used to deliver the first, top-level
// call of an RPC session.
class EntrypointJsRpcTarget final: public JsRpcTargetBase {
 public:
  EntrypointJsRpcTarget(IoContext& ioCtx,
      kj::Maybe<kj::StringPtr> entrypointName,
      Frankenvalue props,
      kj::Maybe<kj::Own<WorkerTracer>> tracer)
      : JsRpcTargetBase(ioCtx),
        // Most of the time we don't really have to clone this but it's hard to fully prove, so
        // let's be safe.
        entrypointName(entrypointName.map([](kj::StringPtr s) { return kj::str(s); })),
        props(kj::mv(props)),
        tracer(kj::mv(tracer)) {}

  TargetInfo getTargetInfo(Worker::Lock& lock, IoContext& ioCtx) override {
    jsg::Lock& js = lock;

    auto handler =
        KJ_REQUIRE_NONNULL(lock.getExportedHandler(entrypointName, kj::mv(props), ioCtx.getActor()),
            "Failed to get handler to worker.");

    if (handler->missingSuperclass) {
      // JS RPC is not enabled on the server side, we cannot call any methods.
      JSG_REQUIRE(FeatureFlags::get(js).getJsRpc(), TypeError,
          "The receiving Durable Object does not support RPC, because its class was not declared "
          "with `extends DurableObject`. In order to enable RPC, make sure your class "
          "extends the special class `DurableObject`, which can be imported from the module "
          "\"cloudflare:workers\".");
    }

    TargetInfo targetInfo{.target = jsg::JsObject(handler->self.getHandle(lock)),
      .envCtx = handler->ctx.map([&](jsg::Ref<ExecutionContext>& execCtx) -> EnvCtx {
      return {
        .env = handler->env.getHandle(js),
        .ctx = lock.getWorker().getIsolate().getApi().wrapExecutionContext(js, execCtx.addRef()),
      };
    })};

    // `targetInfo.envCtx` is present when we're invoking a freestanding function, and therefore
    // `env` and `ctx` need to be passed as parameters. In that case, we our method lookup
    // should obviously permit instance properties, since we expect the export is a plain object.
    // Otherwise, though, the export is a class. In that case, we have set the rule that we will
    // only allow class properties (aka prototype properties) to be accessed, to avoid
    // programmers shooting themselves in the foot by forgetting to make their members private.
    targetInfo.allowInstanceProperties = targetInfo.envCtx != kj::none;

    return targetInfo;
  }

 private:
  kj::Maybe<kj::String> entrypointName;
  Frankenvalue props;
  kj::Maybe<kj::Own<WorkerTracer>> tracer;

  bool isReservedName(kj::StringPtr name) override {
    if (  // "fetch" and "connect" are treated specially on entrypoints.
        name == "fetch" || name == "connect" ||

        // These methods are reserved by the Durable Objects implementation.
        // TODO(someday): Should they be reserved only for Durable Objects, not WorkerEntrypoint?
        name == "alarm" || name == "webSocketMessage" || name == "webSocketClose" ||
        name == "webSocketError" ||

        // dup() is reserved to duplicate the stub itself, pointing to the same object.
        name == "dup" ||

        // All JS classes define a method `constructor` on the prototype, but we don't actually
        // want this to be callable over RPC!
        name == "constructor") {
      return true;
    }
    return false;
  }

  void addTrace(jsg::Lock& js, IoContext& ioctx, kj::StringPtr methodName) override {
    KJ_IF_SOME(t, tracer) {
      t->setEventInfo(ioctx.now(), tracing::JsRpcEventInfo(kj::str(methodName)));
    }
  }
};

// A membrane which wraps the top-level JsRpcTarget of an RPC session on the server side. The
// purpose of this membrane is to allow only a single top-level call, which then gets a
// `CompletionMembrane` wrapped around it. Note that we can't just wrap `CompletionMembrane` around
// the top-level object directly because that capability will not be dropped until the RPC session
// completes, since it is actually returned as the result of the top-level RPC call, but that
// call doesn't return until the `CompletionMembrane` says all capabilities were dropped, so this
// would create a cycle.
class JsRpcSessionCustomEventImpl::ServerTopLevelMembrane final: public capnp::MembranePolicy,
                                                                 public kj::Refcounted {
 public:
  explicit ServerTopLevelMembrane(kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : doneFulfiller(kj::mv(doneFulfiller)) {}
  ~ServerTopLevelMembrane() noexcept(false) {
    KJ_IF_SOME(f, doneFulfiller) {
      f->reject(
          KJ_EXCEPTION(DISCONNECTED, "JS RPC session canceled without calling an RPC method."));
    }
  }

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    auto f = kj::mv(JSG_REQUIRE_NONNULL(
        doneFulfiller, Error, "Only one RPC method call is allowed on this object."));
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
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks) {
  IoContext& ioctx = incomingRequest->getContext();

  incomingRequest->delivered();

  auto [donePromise, doneFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(
      capnp::membrane(kj::heap<EntrypointJsRpcTarget>(ioctx, entrypointName, kj::mv(props),
                          mapAddRef(incomingRequest->getWorkerTracer())),
          kj::refcounted<ServerTopLevelMembrane>(kj::mv(doneFulfiller))));

  KJ_DEFER({
    // waitUntil() should allow extending execution on the server side even when the client
    // disconnects.
    waitUntilTasks.add(incomingRequest->drain().attach(kj::mv(incomingRequest)));
  });

  // `donePromise` resolves once there are no longer any capabilities pointing between the client
  // and server as part of this session.
  co_await donePromise.exclusiveJoin(ioctx.onAbort());

  co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
}

kj::Promise<WorkerInterface::CustomEvent::Result> JsRpcSessionCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
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

  rpc::JsRpcTarget::Client cap = sent.getTopLevel();

  cap = capnp::membrane(kj::mv(cap), kj::refcounted<RevokerMembrane>(kj::mv(revokePaf.promise)));

  // When no more capabilities exist on the connection, we want to proactively cancel the RPC.
  // This is needed in particular for the case where the client is dropped without making any calls
  // at all, e.g. because serializing the arguments failed. Unfortunately, simply dropping the
  // capability obtained through `sent.getTopLevel()` above will not be detected by the server,
  // because this is a pipeline capability on a call that is still running. So, if we don't
  // actually cancel the connection client-side, the server will hang open waiting for the initial
  // top-level call to arrive, and the event will appear never to complete at our end.
  //
  // TODO(cleanup): It feels like there's something wrong with the design here. Can we make this
  //   less ugly?
  auto completionPaf = kj::newPromiseAndFulfiller<void>();
  cap = capnp::membrane(
      kj::mv(cap), kj::refcounted<CompletionMembrane>(kj::mv(completionPaf.fulfiller)));

  this->capFulfiller->fulfill(kj::mv(cap));

  try {
    co_await sent.ignoreResult().exclusiveJoin(kj::mv(completionPaf.promise));
  } catch (...) {
    auto e = kj::getCaughtExceptionAsKj();
    if (revokePaf.fulfiller->isWaiting()) {
      revokePaf.fulfiller->reject(kj::cp(e));
    }
    kj::throwFatalException(kj::mv(e));
  }

  co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
}

// =======================================================================================

jsg::Ref<WorkerEntrypoint> WorkerEntrypoint::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    jsg::Ref<ExecutionContext> ctx,
    jsg::JsObject env) {
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
    jsg::Ref<DurableObjectState> ctx,
    jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* delcare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return jsg::alloc<DurableObjectBase>();
}

jsg::Ref<WorkflowEntrypoint> WorkflowEntrypoint::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    jsg::Ref<ExecutionContext> ctx,
    jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* declare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return jsg::alloc<WorkflowEntrypoint>();
}

};  // namespace workerd::api
