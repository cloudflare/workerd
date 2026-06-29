// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/wrapped-binding.h>
#include <workerd/jsg/setup.h>

namespace workerd::api {

namespace {
constexpr kj::StringPtr WRAPPED_BINDING_ENTRYPOINT = "default"_kj;

jsg::Ref<WrappedBinding> instantiateWrapper(jsg::Lock& js,
    kj::String moduleName,
    jsg::JsObject env,
    const jsg::TypeHandler<jsg::Ref<WrappedBinding>>& selfHandler) {
  auto moduleNs = KJ_REQUIRE_NONNULL(js.resolveInternalModule(moduleName),
      "wrapped binding module can't be resolved (internal modules only): ", moduleName);
  auto fn =
      KJ_REQUIRE_NONNULL(moduleNs.get(js, WRAPPED_BINDING_ENTRYPOINT).tryCast<jsg::JsFunction>(),
          "wrapped binding entrypoint is not a function: ", WRAPPED_BINDING_ENTRYPOINT);
  auto result = KJ_REQUIRE_NONNULL(fn.callNoReceiver(js, env).tryCast<jsg::JsObject>(),
      "wrapped binding entrypoint did not return an object");
  auto binding = KJ_REQUIRE_NONNULL(selfHandler.tryUnwrap(js, result),
      "wrapped binding must inherit from cloudflare-internal:wrapped-binding");
  binding->setWrapperModule(kj::mv(moduleName));
  return binding;
}
}  // namespace

void WrappedBinding::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto& moduleName = KJ_REQUIRE_NONNULL(wrapperModule, "wrapped binding module was not set");
  KJ_REQUIRE_NONNULL(fetcher, "serializable wrapped binding has no fetcher")
      ->serialize(js, serializer);
  serializer.writeLengthDelimited(moduleName);
}

jsg::Ref<WrappedBinding> WrappedBinding::deserialize(jsg::Lock& js,
    rpc::SerializationTag tag,
    jsg::Deserializer& deserializer,
    const jsg::TypeHandler<jsg::Ref<Fetcher>>& innerHandler,
    const jsg::TypeHandler<jsg::Ref<WrappedBinding>>& selfHandler) {
  auto inner = Fetcher::deserialize(js, rpc::SerializationTag::SERVICE_STUB, deserializer);
  auto wrapperModule = deserializer.readLengthDelimitedString();
  auto env = js.obj();
  env.createDataProperty(
      js, js.str(WRAPPED_BINDING_INNER_NAME), jsg::JsValue(innerHandler.wrap(js, kj::mv(inner))));

  // All input has been read and no deserializer state enters this scope. The internal wrapper may
  // invoke user JavaScript indirectly, so keep the exception to V8's guard tightly scoped.
  {
    v8::Isolate::AllowJavascriptExecutionScope allowJs(js.v8Isolate);
    return instantiateWrapper(js, kj::mv(wrapperModule), env, selfHandler);
  }
}

}  // namespace workerd::api
