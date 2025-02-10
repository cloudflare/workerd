#include "modules.h"

#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>

namespace workerd::api {

kj::Maybe<jsg::JsObject> EnvModule::getCurrent(jsg::Lock& js) {
  auto& key = jsg::IsolateBase::from(js.v8Isolate).getEnvAsyncContextKey();
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_SOME(value, frame.get(key)) {
      auto handle = value.getHandle(js);
      if (handle->IsObject()) {
        return jsg::JsObject(handle.As<v8::Object>());
      }
    }
  }
  // If the compat flag is set to disable importable env, then this
  // will return nothing.
  if (FeatureFlags::get(js).getDisableImportableEnv()) return kj::none;

  // Otherwise, fallback to provide the stored environment.
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

}  // namespace workerd::api
