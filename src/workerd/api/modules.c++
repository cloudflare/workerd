#include "modules.h"

#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>

namespace workerd::api {

kj::Maybe<jsg::JsObject> EnvModule::getCurrentEnv(jsg::Lock& js) {
  auto& key = jsg::IsolateBase::from(js.v8Isolate).getEnvAsyncContextKey();
  // Check async context first - withEnv() overrides take precedence over the disable flag.
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_SOME(value, frame.get(key)) {
      auto handle = value.getHandle(js);
      if (handle->IsObject()) {
        return jsg::JsObject(handle.As<v8::Object>());
      }
      if (FeatureFlags::get(js).getEnvModuleNullableSupport() && handle->IsNullOrUndefined()) {
        return kj::none;
      }
    }
  }
  if (FeatureFlags::get(js).getDisableImportableEnv()) return kj::none;

  return js.getWorkerEnv().map([&](const jsg::V8Ref<v8::Object>& val) -> jsg::JsObject {
    return jsg::JsObject(val.getHandle(js));
  });
}

jsg::JsRef<jsg::JsValue> EnvModule::withEnv(
    jsg::Lock& js, jsg::Value newEnv, jsg::Function<jsg::JsRef<jsg::JsValue>()> fn) {
  auto& key = jsg::IsolateBase::from(js.v8Isolate).getEnvAsyncContextKey();
  jsg::AsyncContextFrame::StorageScope storage(js, key, kj::mv(newEnv));
  return js.tryCatch([&]() mutable -> jsg::JsRef<jsg::JsValue> { return fn(js); },
      [&](jsg::Value&& exception) mutable -> jsg::JsRef<jsg::JsValue> {
    js.throwException(kj::mv(exception));
  });
}

kj::Maybe<jsg::JsObject> EnvModule::getCurrentExports(jsg::Lock& js) {
  auto& key = jsg::IsolateBase::from(js.v8Isolate).getExportsAsyncContextKey();
  // Check async context first - withExports() overrides take precedence over the disable flags.
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_SOME(value, frame.get(key)) {
      auto handle = value.getHandle(js);
      if (handle->IsObject()) {
        return jsg::JsObject(handle.As<v8::Object>());
      }
      if (FeatureFlags::get(js).getEnvModuleNullableSupport() && handle->IsNullOrUndefined()) {
        return kj::none;
      }
    }
  }
  if (FeatureFlags::get(js).getDisableImportableEnv()) {
    return kj::none;
  }

  return js.getWorkerExports().map([&](const jsg::V8Ref<v8::Object>& val) -> jsg::JsObject {
    return jsg::JsObject(val.getHandle(js));
  });
}

jsg::JsRef<jsg::JsValue> EnvModule::withExports(
    jsg::Lock& js, jsg::Value newExports, jsg::Function<jsg::JsRef<jsg::JsValue>()> fn) {
  auto& key = jsg::IsolateBase::from(js.v8Isolate).getExportsAsyncContextKey();
  jsg::AsyncContextFrame::StorageScope storage(js, key, kj::mv(newExports));
  return js.tryCatch([&]() mutable -> jsg::JsRef<jsg::JsValue> { return fn(js); },
      [&](jsg::Value&& exception) mutable -> jsg::JsRef<jsg::JsValue> {
    js.throwException(kj::mv(exception));
  });
}

jsg::JsRef<jsg::JsValue> EnvModule::withEnvAndExports(jsg::Lock& js,
    jsg::Value newEnv,
    jsg::Value newExports,
    jsg::Function<jsg::JsRef<jsg::JsValue>()> fn) {
  auto& envKey = jsg::IsolateBase::from(js.v8Isolate).getEnvAsyncContextKey();
  auto& exportsKey = jsg::IsolateBase::from(js.v8Isolate).getExportsAsyncContextKey();
  jsg::AsyncContextFrame::StorageScope envStorage(js, envKey, kj::mv(newEnv));
  jsg::AsyncContextFrame::StorageScope exportsStorage(js, exportsKey, kj::mv(newExports));
  return js.tryCatch([&]() mutable -> jsg::JsRef<jsg::JsValue> { return fn(js); },
      [&](jsg::Value&& exception) mutable -> jsg::JsRef<jsg::JsValue> {
    js.throwException(kj::mv(exception));
  });
}

}  // namespace workerd::api
