// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workers-module.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>

namespace workerd::api {

jsg::Ref<WorkerEntrypoint> WorkerEntrypoint::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args, jsg::JsObject ctx, jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* declare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return js.alloc<WorkerEntrypoint>();
}

jsg::Ref<DurableObjectBase> DurableObjectBase::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    jsg::Ref<DurableObjectState> ctx,
    jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* declare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return js.alloc<DurableObjectBase>();
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
  return js.alloc<WorkflowEntrypoint>();
}

void EntrypointsModule::waitUntil(kj::Promise<void> promise) {
  // No need to check if IoContext::hasCurrent since current() will throw
  // if there is no active request.
  IoContext::current().addWaitUntil(kj::mv(promise));
}

jsg::Optional<jsg::Ref<CacheContext>> EntrypointsModule::getCtxCache(jsg::Lock& js) {
  // Delegate to the embedding application's getCtxCacheProperty() hook, which returns a
  // CacheContext (e.g. CachePurge in edgeworker) when cache is enabled for this pipeline.
  // Returns kj::none when there is no active request or cache is not available.
  if (IoContext::hasCurrent()) {
    return Worker::Isolate::from(js).getApi().getCtxCacheProperty(js);
  }
  return kj::none;
}

void EntrypointsModule::abortIsolate(jsg::Lock& js, jsg::Optional<kj::String> reason) {
  auto& r = reason.orDefault(nullptr);
  KJ_IF_SOME(context, IoContext::tryCurrent()) {
    context.abortIsolate(r);
  }
  js.terminateExecutionNow();
}

jsg::JsValue EntrypointsModule::retryable(
    jsg::Lock& js, jsg::JsValue value, jsg::JsObject context) {
  v8::Local<v8::Function> function = JSG_REQUIRE_NONNULL(value.tryCast<jsg::JsFunction>(),
      TypeError, "@retryable can only decorate public instance methods.");
  JSG_REQUIRE(context.get(js, "kind"_kj).strictEquals(js.strIntern("method"_kj)) &&
          context.get(js, "static"_kj).strictEquals(js.boolean(false)) &&
          context.get(js, "private"_kj).strictEquals(js.boolean(false)),
      TypeError, "@retryable can only decorate public instance methods.");

  jsg::JsObject(function).setPrivate(js, RETRYABLE_METHOD_PRIVATE_KEY, js.boolean(true));
  return value;
}

jsg::JsSymbol EntrypointsModule::getRestoreSymbol(jsg::Lock& js) {
  return js.symbolInternal("cloudflare:workers:restore");
}

}  // namespace workerd::api
