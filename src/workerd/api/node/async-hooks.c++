// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-hooks.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

namespace workerd::api::node {

namespace {
// If there is a current IoContext, then it is possible/likely that the current
// AsyncContextFrame is storing values that are bound to that IoContext. In
// that case, we protect against entering the frame from a different IoContext.
// To do this we capture the current IoContext's unique id and compare it
// against the current IoContext where the frame is entered.
// Capturing the id (rather than a weak reference) is cheaper and avoids
// keeping any state tied to the IoContext alive after it is destroyed:
// because ids are never reused, a stale id can never match a later context.
kj::Maybe<IoContext::Id> getIoContextId(jsg::Lock& js) {
  if (FeatureFlags::get(js).getBindAsyncLocalStorageSnapshot() && IoContext::hasCurrent()) {
    return IoContext::current().getId();
  }
  return kj::none;
}

void validateIoContext(jsg::Lock&, kj::Maybe<IoContext::Id> maybeIoContextId) {
  static constexpr auto kErrorMessage =
      "Cannot call this AsyncLocalStorage bound function outside of the "
      "request in which it was created."_kj;

  KJ_IF_SOME(originIoContextId, maybeIoContextId) {
    JSG_REQUIRE(originIoContextId.isCurrent(), Error, kErrorMessage);
  }
}

// The functions AsyncLocalStorage.bind()/snapshot() and AsyncResource.bind() return are made with
// jsg::newBoundFunction(): a static callback with the per-function state bound as its four leading
// arguments [fn, thisArg, originIoContextId, frame]. `fn` is the wrapped function (undefined for
// the snapshot form, which takes it as its first call argument), `originIoContextId` is undefined
// or the BigInt of the IoContext::Id the function was created in, and `frame` is undefined (root)
// or the opaque wrapper of the jsg::AsyncContextFrame to enter. No C++ state is captured, so such
// a function can be created while preparing a startup snapshot: one bound in the root frame
// transfers through it, while one bound inside a frame carries the frame's opaque wrapper, which
// has no transfer.
//
// Positions of that state among the callback's arguments; the caller's follow at kBoundStateSize.
constexpr int kBoundFnSlot = 0;
constexpr int kBoundThisSlot = 1;
constexpr int kBoundIoContextIdSlot = 2;
constexpr int kBoundFrameSlot = 3;
constexpr int kBoundStateSize = 4;

v8::Local<v8::Function> makeBoundFunction(jsg::Lock& js,
    v8::FunctionCallback callback,
    v8::Local<v8::Value> fn,
    kj::Maybe<v8::Local<v8::Value>> thisArg,
    kj::Maybe<IoContext::Id> originIoContextId,
    kj::Maybe<jsg::AsyncContextFrame&> frame) {
  auto isolate = js.v8Isolate;
  v8::Local<v8::Value> state[kBoundStateSize];
  state[kBoundFnSlot] = fn;
  state[kBoundThisSlot] = thisArg.orDefault(js.v8Context()->Global());
  state[kBoundIoContextIdSlot] = v8::Undefined(isolate);
  KJ_IF_SOME(id, originIoContextId) {
    state[kBoundIoContextIdSlot] = v8::BigInt::NewFromUnsigned(isolate, id.toRaw());
  }
  state[kBoundFrameSlot] = v8::Undefined(isolate);
  KJ_IF_SOME(f, frame) {
    state[kBoundFrameSlot] = f.getJSWrapper(js);
  }
  return jsg::newBoundFunction(js, callback, state);
}

struct BoundFunctionState {
  v8::Local<v8::Value> fn;
  v8::Local<v8::Value> thisArg;
  kj::Maybe<IoContext::Id> originIoContextId;
  kj::Maybe<jsg::AsyncContextFrame&> frame;
};

BoundFunctionState readBoundFunctionState(
    jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
  // jsg::newBoundFunction() only ever calls the callback with the state in front.
  KJ_ASSERT(info.Length() >= kBoundStateSize);
  BoundFunctionState state{.fn = info[kBoundFnSlot], .thisArg = info[kBoundThisSlot]};
  auto id = info[kBoundIoContextIdSlot];
  if (id->IsBigInt()) {
    state.originIoContextId = IoContext::Id::fromRaw(id.As<v8::BigInt>()->Uint64Value());
  }
  state.frame = jsg::AsyncContextFrame::tryUnwrap(js.v8Isolate, info[kBoundFrameSlot]);
  return state;
}

// Behind AsyncLocalStorage.bind() and AsyncResource.bind(): calls `fn` on `thisArg` in `frame`.
void boundFunctionCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  jsg::liftKj(info, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(info.GetIsolate());
    auto state = readBoundFunctionState(js, info);
    KJ_ASSERT(state.fn->IsFunction());
    validateIoContext(js, state.originIoContextId);

    v8::LocalVector<v8::Value> argv(js.v8Isolate, info.Length() - kBoundStateSize);
    for (int n = kBoundStateSize; n < info.Length(); n++) {
      argv[n - kBoundStateSize] = info[n];
    }

    jsg::AsyncContextFrame::Scope scope(js, state.frame);
    return jsg::check(
        state.fn.As<v8::Function>()->Call(js.v8Context(), state.thisArg, argv.size(), argv.data()));
  });
}

// Behind AsyncLocalStorage.snapshot(): calls its first argument with the rest in `frame`.
void snapshotFunctionCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  jsg::liftKj(info, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(info.GetIsolate());
    auto state = readBoundFunctionState(js, info);
    validateIoContext(js, state.originIoContextId);
    auto context = js.v8Context();
    constexpr int kCallbackSlot = kBoundStateSize;
    JSG_REQUIRE(info.Length() > kCallbackSlot && info[kCallbackSlot]->IsFunction(), TypeError,
        "The first argument must be a function");
    auto fn = info[kCallbackSlot].As<v8::Function>();

    v8::LocalVector<v8::Value> argv(js.v8Isolate, info.Length() - kCallbackSlot - 1);
    for (int n = kCallbackSlot + 1; n < info.Length(); n++) {
      argv[n - kCallbackSlot - 1] = info[n];
    }

    jsg::AsyncContextFrame::Scope scope(js, state.frame);
    return jsg::check(fn->Call(context, context->Global(), argv.size(), argv.data()));
  });
}

}  // namespace

jsg::Ref<AsyncLocalStorage> AsyncLocalStorage::constructor(
    jsg::Lock& js, jsg::Optional<AsyncLocalStorage::AsyncLocalStorageOptions> options) {
  return js.alloc<AsyncLocalStorage>(kj::mv(options));
}

v8::Local<v8::Value> AsyncLocalStorage::run(jsg::Lock& js,
    v8::Local<v8::Value> store,
    jsg::Function<v8::Local<v8::Value>(jsg::Arguments<jsg::Value>)> callback,
    jsg::Arguments<jsg::Value> args) {
  callback.setReceiver(js.v8Ref<v8::Value>(js.v8Context()->Global()));
  jsg::AsyncContextFrame::StorageScope scope(js, key.addRef(), js.v8Ref(store));
  return callback(js, kj::mv(args));
}

v8::Local<v8::Value> AsyncLocalStorage::exit(jsg::Lock& js,
    jsg::Function<v8::Local<v8::Value>(jsg::Arguments<jsg::Value>)> callback,
    jsg::Arguments<jsg::Value> args) {
  // Node.js defines exit as running "a function synchronously outside of a context".
  // It goes on to say that the store is not accessible within the callback or the
  // asynchronous operations created within the callback. Any getStore() call done
  // within the callback function will always return undefined... except if run() is
  // called which implicitly enables the context again within that scope.
  //
  // We do not have to emulate Node.js enable/disable behavior since we are not
  // implementing the enterWith/disable methods. We can emulate the correct
  // behavior simply by calling run with the store value set to undefined, which
  // will propagate correctly.
  return run(js, js.undefined(), kj::mv(callback), kj::mv(args));
}

v8::Local<v8::Value> AsyncLocalStorage::getStore(jsg::Lock& js) {
  KJ_IF_SOME(context, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_SOME(value, context.get(*key)) {
      return value.getHandle(js);
    }
  }
  KJ_IF_SOME(value, defaultValue) {
    return value.getHandle(js);
  }
  return js.undefined();
}

kj::StringPtr AsyncLocalStorage::getName() {
  KJ_IF_SOME(n, name) {
    return n.asPtr();
  }
  return nullptr;
}

v8::Local<v8::Function> AsyncLocalStorage::bind(jsg::Lock& js, v8::Local<v8::Function> fn) {
  auto frame = jsg::AsyncContextFrame::current(js);
  // A function bound in the root frame carries no request-bound storage, so it may be called
  // from any request.
  kj::Maybe<IoContext::Id> originIoContextId;
  if (frame != kj::none) {
    originIoContextId = getIoContextId(js);
  }
  return makeBoundFunction(js, &boundFunctionCallback, fn, kj::none, originIoContextId, frame);
}

v8::Local<v8::Function> AsyncLocalStorage::snapshot(jsg::Lock& js) {
  return makeBoundFunction(js, &snapshotFunctionCallback, v8::Undefined(js.v8Isolate), kj::none,
      getIoContextId(js), jsg::AsyncContextFrame::current(js));
}

namespace {
kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> tryGetFrameRef(jsg::Lock& js) {
  return jsg::AsyncContextFrame::current(js).map(
      [](jsg::AsyncContextFrame& frame) { return frame.addRef(); });
}
}  // namespace

AsyncResource::AsyncResource(jsg::Lock& js): frame(tryGetFrameRef(js)) {
  if (frame != kj::none) {
    originIoContextId = getIoContextId(js);
  }
}

jsg::Ref<AsyncResource> AsyncResource::constructor(
    jsg::Lock& js, jsg::Optional<kj::String> type, jsg::Optional<Options> options) {
  // The type and options are required as part of the Node.js API compatibility
  // but our implementation does not currently make use of them at all. It is OK
  // for us to silently ignore both here.
  return js.alloc<AsyncResource>(js);
}

v8::Local<v8::Function> AsyncResource::staticBind(jsg::Lock& js,
    v8::Local<v8::Function> fn,
    jsg::Optional<kj::String> type,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler) {
  return AsyncResource::constructor(js, kj::mv(type).orDefault([] {
    return kj::str("AsyncResource");
  }))->bind(js, fn, thisArg, handler);
}

kj::Maybe<jsg::AsyncContextFrame&> AsyncResource::getFrame() {
  return frame.map([](jsg::Ref<jsg::AsyncContextFrame>& frame) -> jsg::AsyncContextFrame& {
    return *(frame.get());
  });
}

v8::Local<v8::Function> AsyncResource::bind(jsg::Lock& js,
    v8::Local<v8::Function> fn,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler) {
  // originIoContextId is only ever set together with a captured frame, so a root-frame resource
  // yields a function callable from any request.
  auto bound =
      makeBoundFunction(js, &boundFunctionCallback, fn, thisArg, originIoContextId, getFrame());

  // Per Node.js documentation (https://nodejs.org/dist/latest-v19.x/docs/api/async_context.html#asyncresourcebindfn-thisarg), the returned function "will have an
  // asyncResource property referencing the AsyncResource to which the function
  // is bound".
  js.v8Set(bound, "asyncResource"_kj, handler.wrap(js, JSG_THIS));
  return bound;
}

v8::Local<v8::Value> AsyncResource::runInAsyncScope(jsg::Lock& js,
    jsg::Function<v8::Local<v8::Value>(jsg::Arguments<jsg::Value>)> fn,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    jsg::Arguments<jsg::Value> args) {
  v8::Local<v8::Value> receiver = js.v8Context()->Global();
  KJ_IF_SOME(arg, thisArg) {
    receiver = arg;
  }
  fn.setReceiver(js.v8Ref<v8::Value>(receiver));
  validateIoContext(js, originIoContextId);
  jsg::AsyncContextFrame::Scope scope(js, getFrame());
  return fn(js, kj::mv(args));
}

kj::Arc<jsg::AsyncContextFrame::StorageKey> AsyncLocalStorage::getKey() {
  return key.addRef();
}

}  // namespace workerd::api::node
