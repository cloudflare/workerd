// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "url-bridge.h"

#include <workerd/api/per-isolate-bootstrap.h>

namespace workerd::api {

namespace {

// Set by the constructors in src/per_isolate/url/url.ts.
constexpr kj::StringPtr URL_BRAND = "workerd:URL"_kj;
constexpr kj::StringPtr SEARCH_PARAMS_BRAND = "workerd:URLSearchParams"_kj;

kj::Maybe<jsg::JsObject> tryBranded(jsg::Lock& js, jsg::JsValue value, kj::StringPtr brand) {
  KJ_IF_SOME(object, value.tryCast<jsg::JsObject>()) {
    if (!v8::Local<v8::Value>(object)->IsProxy() && object.getPrivate(js, brand).isTrue()) {
      return object;
    }
  }
  return kj::none;
}

kj::String callCppExport(jsg::Lock& js, kj::StringPtr name, const jsg::JsRef<jsg::JsObject>& arg) {
  KJ_ASSERT(!js.isJavascriptExecutionDisallowed(),
      "attempted to call into the TypeScript URL implementation during a no-JS scope", name);
  auto function = KJ_REQUIRE_NONNULL(tryGetBootstrapFunction(js, "url/cpp_exports"_kj, name),
      "missing url/cpp_exports function", name);
  auto result = function.call(js, js.undefined(), arg.getHandle(js));
  return KJ_REQUIRE_NONNULL(result.tryCast<jsg::JsString>(), "expected a string", name)
      .toString(js);
}

}  // namespace

kj::String JsUrl::getHref(jsg::Lock& js) const {
  return callCppExport(js, "urlHref"_kj, object);
}

kj::Maybe<JsUrl> JsUrl::tryUnwrap(jsg::Lock& js, jsg::JsValue value) {
  return tryBranded(js, value, URL_BRAND).map([&](jsg::JsObject object) {
    return JsUrl(object.addRef(js));
  });
}

kj::String JsUrlSearchParams::serialize(jsg::Lock& js) const {
  return callCppExport(js, "searchParamsSerialize"_kj, object);
}

kj::Maybe<JsUrlSearchParams> JsUrlSearchParams::tryUnwrap(jsg::Lock& js, jsg::JsValue value) {
  return tryBranded(js, value, SEARCH_PARAMS_BRAND).map([&](jsg::JsObject object) {
    return JsUrlSearchParams(object.addRef(js));
  });
}

}  // namespace workerd::api
