// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "util.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/jsg.h>

#include <kj/vector.h>

namespace workerd::api::node {

jsg::JsArray UtilModule::getOwnNonIndexProperties(jsg::Lock& js, jsg::JsObject value, int filter) {
  auto propertyFilter = static_cast<jsg::PropertyFilter>(filter);
  return value.getPropertyNames(
      js, jsg::KeyCollectionFilter::OWN_ONLY, propertyFilter, jsg::IndexFilter::SKIP_INDICES);
}

jsg::Optional<UtilModule::PromiseDetails> UtilModule::getPromiseDetails(
    jsg::Lock& js, jsg::JsValue value) {
  auto promise = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsPromise>(), kj::none);
  auto state = promise.state();
  if (state != jsg::PromiseState::PENDING) {
    auto result = promise.result();
    return PromiseDetails{
      .state = state,
      .result = jsg::JsRef(js, result),
    };
  } else {
    return PromiseDetails{
      .state = state,
      .result = kj::none,
    };
  }
}

jsg::Optional<UtilModule::ProxyDetails> UtilModule::getProxyDetails(
    jsg::Lock& js, jsg::JsValue value) {
  auto proxy = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsProxy>(), kj::none);
  auto target = proxy.target();
  auto handler = proxy.handler();
  return ProxyDetails{
    .target = jsg::JsRef(js, target),
    .handler = jsg::JsRef(js, handler),
  };
}

jsg::Optional<UtilModule::PreviewedEntries> UtilModule::previewEntries(
    jsg::Lock& js, jsg::JsValue value) {
  auto object = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsObject>(), kj::none);
  bool isKeyValue;
  auto entries = object.previewEntries(&isKeyValue);
  return PreviewedEntries{
    .entries = jsg::JsRef(js, entries),
    .isKeyValue = isKeyValue,
  };
}

jsg::JsString UtilModule::getConstructorName(jsg::Lock& js, jsg::JsObject value) {
  return js.str(value.getConstructorName());
}

#define V(Type)                                                                                    \
  bool UtilModule::is##Type(jsg::JsValue value) {                                                  \
    return value.is##Type();                                                                       \
  };
JS_UTIL_IS_TYPES(V)
#undef V
bool UtilModule::isAnyArrayBuffer(jsg::JsValue value) {
  return value.isArrayBuffer() || value.isSharedArrayBuffer();
}
bool UtilModule::isBoxedPrimitive(jsg::JsValue value) {
  return value.isNumberObject() || value.isStringObject() || value.isBooleanObject() ||
      value.isBigIntObject() || value.isSymbolObject();
}

jsg::Name UtilModule::getResourceTypeInspect(jsg::Lock& js) {
  return js.newApiSymbol("kResourceTypeInspect"_kj);
}

kj::Array<UtilModule::CallSiteEntry> UtilModule::getCallSites(
    jsg::Lock& js, jsg::Optional<int> frames) {
  KJ_IF_SOME(f, frames) {
    JSG_REQUIRE(f >= 1 && f <= 200, Error, "Frame count should be between 1 and 200 inclusive."_kj);
  }

  auto stack = v8::StackTrace::CurrentStackTrace(js.v8Isolate, frames.orDefault(10) + 1);
  const int frameCount = stack->GetFrameCount();
  auto objects = kj::Vector<CallSiteEntry>();
  objects.reserve(frameCount - 1);

  for (int i = 0; i < frameCount; ++i) {
    auto stack_frame = stack->GetFrame(js.v8Isolate, i);

    auto function_name = stack_frame->GetFunctionName();
    auto script_name = stack_frame->GetScriptName();

    if (!function_name.IsEmpty() && !script_name.IsEmpty()) {

      objects.add(CallSiteEntry{
        .functionName = js.toString(function_name),
        .scriptName = js.toString(script_name),
        .lineNumber = stack_frame->GetLineNumber(),
        // Node.js originally implemented the experimental API using the "column" field
        // then later renamed it to columnNumber. We had already implemented the API
        // using column. To ensure backwards compat without the complexity of a compat
        // flag, we just export both.
        .columnNumber = stack_frame->GetColumn(),
        .column = stack_frame->GetColumn(),
      });
    }
  }

  return objects.releaseAsArray();
}

}  // namespace workerd::api::node
