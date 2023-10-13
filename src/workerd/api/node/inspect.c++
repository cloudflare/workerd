// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "inspect.h"
#include <kj/encoding.h>

namespace workerd::api {

jsg::JsArray InspectModule::getOwnNonIndexProperties(jsg::Lock& js, jsg::JsObject value,
                                                       int filter) {
  auto propertyFilter = static_cast<jsg::PropertyFilter>(filter);
  return value.getPropertyNames(js, jsg::KeyCollectionFilter::OWN_ONLY, propertyFilter,
                                  jsg::IndexFilter::SKIP_INDICES);
}

jsg::Optional<InspectModule::PromiseDetails> InspectModule::getPromiseDetails(jsg::JsValue value) {
  auto promise = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsPromise>(), kj::none);
  auto state = promise.state();
  if (state != jsg::PromiseState::PENDING) {
    auto result = promise.result();
    return PromiseDetails { .state = state, .result = result };
  } else {
    return PromiseDetails { .state = state, .result = kj::none };
  }
}

jsg::Optional<InspectModule::ProxyDetails> InspectModule::getProxyDetails(jsg::JsValue value) {
  auto proxy = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsProxy>(), kj::none);
  auto target = proxy.target();
  auto handler = proxy.handler();
  return ProxyDetails { .target = target, .handler = handler };
}

jsg::Optional<InspectModule::PreviewedEntries> InspectModule::previewEntries(jsg::JsValue value) {
  auto object = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsObject>(), kj::none);
  bool isKeyValue;
  auto entries = object.previewEntries(&isKeyValue);
  return PreviewedEntries { .entries = entries, .isKeyValue = isKeyValue };
}

jsg::JsString InspectModule::getConstructorName(jsg::Lock& js, jsg::JsObject value) {
  return js.str(value.getConstructorName());
}

#define V(Type) bool InspectModule::is##Type(jsg::JsValue value) { return value.is##Type(); };
  JS_INSPECT_IS_TYPES(V)
#undef V
bool InspectModule::isAnyArrayBuffer(jsg::JsValue value) {
  return value.isArrayBuffer() || value.isSharedArrayBuffer();
}
bool InspectModule::isBoxedPrimitive(jsg::JsValue value) {
  return value.isNumberObject() ||
         value.isStringObject() ||
         value.isBooleanObject() ||
         value.isBigIntObject() ||
         value.isSymbolObject();
}

jsg::Name InspectModule::getResourceTypeInspect(jsg::Lock& js) {
  return js.newApiSymbol("kResourceTypeInspect"_kj);
}

}
