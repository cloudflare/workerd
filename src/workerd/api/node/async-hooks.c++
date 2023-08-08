// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-hooks.h"
#include <kj/vector.h>

namespace workerd::api::node {

jsg::Ref<AsyncLocalStorage> AsyncLocalStorage::constructor(jsg::Lock& js) {
  return jsg::alloc<AsyncLocalStorage>();
}

v8::Local<v8::Value> AsyncLocalStorage::run(
    jsg::Lock& js,
    v8::Local<v8::Value> store,
    v8::Local<v8::Function> callback,
    jsg::Varargs args) {
  kj::Vector<v8::Local<v8::Value>> argv(args.size());
  for (auto arg : args) {
    argv.add(arg.getHandle(js));
  }

  auto context = js.v8Context();

  jsg::AsyncContextFrame::StorageScope scope(js, *key, js.v8Ref(store));

  return jsg::check(callback->Call(
      context,
      context->Global(),
      argv.size(),
      argv.begin()));
}

v8::Local<v8::Value> AsyncLocalStorage::exit(
    jsg::Lock& js,
    v8::Local<v8::Function> callback,
    jsg::Varargs args) {
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
  return run(js, js.v8Undefined(), callback, kj::mv(args));
}

v8::Local<v8::Value> AsyncLocalStorage::getStore(jsg::Lock& js) {
  KJ_IF_MAYBE(context, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_MAYBE(value, context->get(*key)) {
      return value->getHandle(js);
    }
  }
  return js.v8Undefined();
}

v8::Local<v8::Function> AsyncLocalStorage::bind(jsg::Lock& js, v8::Local<v8::Function> fn) {
  KJ_IF_MAYBE(frame, jsg::AsyncContextFrame::current(js)) {
    return frame->wrap(js, fn);
  } else {
    return jsg::AsyncContextFrame::wrapRoot(js, fn);
  }
}

v8::Local<v8::Function> AsyncLocalStorage::snapshot(jsg::Lock& js) {
  return jsg::AsyncContextFrame::wrapSnapshot(js);
}

namespace {
kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> tryGetFrameRef(jsg::Lock& js) {
  return jsg::AsyncContextFrame::current(js).map(
      [](jsg::AsyncContextFrame& frame) { return frame.addRef(); });
}
}  // namespace

AsyncResource::AsyncResource(jsg::Lock& js) : frame(tryGetFrameRef(js)) {}

jsg::Ref<AsyncResource> AsyncResource::constructor(
    jsg::Lock& js,
    jsg::Optional<kj::String> type,
    jsg::Optional<Options> options) {
  // The type and options are required as part of the Node.js API compatibility
  // but our implementation does not currently make use of them at all. It is ok
  // for us to silently ignore both here.
  return jsg::alloc<AsyncResource>(js);
}

v8::Local<v8::Function> AsyncResource::staticBind(
    jsg::Lock& js,
    v8::Local<v8::Function> fn,
    jsg::Optional<kj::String> type,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler) {
  return AsyncResource::constructor(js, kj::mv(type)
      .orDefault([] { return kj::str("AsyncResource"); }))
          ->bind(js, fn, thisArg, handler);
}

kj::Maybe<jsg::AsyncContextFrame&> AsyncResource::getFrame() {
  return frame.map([](jsg::Ref<jsg::AsyncContextFrame>& frame)
      -> jsg::AsyncContextFrame& {
    return *(frame.get());
  });
}

v8::Local<v8::Function> AsyncResource::bind(
    jsg::Lock& js,
    v8::Local<v8::Function> fn,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler) {
  v8::Local<v8::Function> bound;
  KJ_IF_MAYBE(frame, getFrame()) {
    bound = frame->wrap(js, fn, thisArg);
  } else {
    bound = jsg::AsyncContextFrame::wrapRoot(js, fn, thisArg);
  }

  // Per Node.js documentation (https://nodejs.org/dist/latest-v19.x/docs/api/async_context.html#asyncresourcebindfn-thisarg), the returned function "will have an
  // asyncResource property referencing the AsyncResource to which the function
  // is bound".
  js.v8Set(bound, "asyncResource"_kj, handler.wrap(js, JSG_THIS));
  return bound;
}

v8::Local<v8::Value> AsyncResource::runInAsyncScope(
    jsg::Lock& js,
    v8::Local<v8::Function> fn,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    jsg::Varargs args) {
  kj::Vector<v8::Local<v8::Value>> argv(args.size());
  for (auto arg : args) {
    argv.add(arg.getHandle(js));
  }

  auto context = js.v8Context();

  jsg::AsyncContextFrame::Scope scope(js, getFrame());

  return jsg::check(fn->Call(
      context,
      thisArg.orDefault(context->Global()),
      argv.size(),
      argv.begin()));
}

kj::Own<jsg::AsyncContextFrame::StorageKey> AsyncLocalStorage::getKey() {
  return kj::addRef(*key);
}

}  // namespace workerd::api::node
