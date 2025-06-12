// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-hooks.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

namespace workerd::api::node {

namespace {
// If there is a current IoContext, then it is possible/likely that the
// current AsyncContextFrame is storing values that are bound to that
// IoContext. In that case, we want to protect against the case where
// the returned snapshot function is called from a different IoContext.
// To do this we will capture a weak reference to the current IoContext
// and check it against the current IoContext where the snapshot function
// is invoked.
jsg::Function<void()> getValidator(jsg::Lock& js) {
  kj::Maybe<kj::Own<IoContext::WeakRef>> maybeIoContext;
  if (FeatureFlags::get(js).getBindAsyncLocalStorageSnapshot() && IoContext::hasCurrent()) {
    // We use a weak reference to the IoContext because the current IoContext
    // may be destroyed before the snapshot function is called.
    maybeIoContext = IoContext::current().getWeakRef();
  }

  static constexpr auto kErrorMessage =
      "Cannot call this AsyncLocalStorage bound function outside of the "
      "request in which it was created."_kj;

  return [maybeIoContext = kj::mv(maybeIoContext)](jsg::Lock&) {
    KJ_IF_SOME(originIoContext, maybeIoContext) {
      // We had an IoContext when we created the snapshot function.
      // If it is not the current IoContext, or if there is no current
      // IoContext, or if the captured IoContext has been destroyed,
      // we throw an error.
      JSG_REQUIRE(IoContext::hasCurrent() && originIoContext->isValid(), Error, kErrorMessage);
      originIoContext->runIfAlive([&](IoContext& otherContext) {
        JSG_REQUIRE(&otherContext == &IoContext::current(), Error, kErrorMessage);
      });
    }
  };
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
  jsg::AsyncContextFrame::StorageScope scope(js, *key, js.v8Ref(store));
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
  return run(js, js.v8Undefined(), kj::mv(callback), kj::mv(args));
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
  return js.v8Undefined();
}

kj::StringPtr AsyncLocalStorage::getName() {
  KJ_IF_SOME(n, name) {
    return n.asPtr();
  }
  return nullptr;
}

v8::Local<v8::Function> AsyncLocalStorage::bind(jsg::Lock& js, v8::Local<v8::Function> fn) {
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    return frame.wrap(js, fn, getValidator(js));
  } else {
    return jsg::AsyncContextFrame::wrapRoot(js, fn);
  }
}

v8::Local<v8::Function> AsyncLocalStorage::snapshot(jsg::Lock& js) {
  return jsg::AsyncContextFrame::wrapSnapshot(js, getValidator(js));
}

namespace {
kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> tryGetFrameRef(jsg::Lock& js) {
  return jsg::AsyncContextFrame::current(js).map(
      [](jsg::AsyncContextFrame& frame) { return frame.addRef(); });
}
}  // namespace

AsyncResource::AsyncResource(jsg::Lock& js): frame(tryGetFrameRef(js)) {}

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
  v8::Local<v8::Function> bound;
  KJ_IF_SOME(frame, getFrame()) {
    bound = frame.wrap(js, fn, getValidator(js), thisArg);
  } else {
    bound = jsg::AsyncContextFrame::wrapRoot(js, fn, thisArg);
  }

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
  jsg::AsyncContextFrame::Scope scope(js, getFrame());
  return fn(js, kj::mv(args));
}

kj::Own<jsg::AsyncContextFrame::StorageKey> AsyncLocalStorage::getKey() {
  return kj::addRef(*key);
}

}  // namespace workerd::api::node
