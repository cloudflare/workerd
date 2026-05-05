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

MIMEParams::MIMEParams(kj::Maybe<MimeType&> mimeType): mimeType(mimeType) {}

// Oddly, Node.js allows creating MIMEParams directly but it's not actually
// functional. But, to match, we'll go ahead and allow it.
jsg::Ref<MIMEParams> MIMEParams::constructor(jsg::Lock& js) {
  return js.alloc<MIMEParams>(kj::none);
}

void MIMEParams::delete_(kj::String name) {
  KJ_IF_SOME(inner, mimeType) {
    inner.eraseParam(name);
  }
}

kj::Maybe<kj::StringPtr> MIMEParams::get(kj::String name) {
  KJ_IF_SOME(inner, mimeType) {
    return inner.params().find(name);
  }
  return kj::none;
}

bool MIMEParams::has(kj::String name) {
  KJ_IF_SOME(inner, mimeType) {
    return inner.params().find(name) != kj::none;
  }
  return false;
}

void MIMEParams::set(kj::String name, kj::String value) {
  KJ_IF_SOME(inner, mimeType) {
    JSG_REQUIRE(inner.addParam(name, value), TypeError, "Not a valid MIME parameter");
  }
}

kj::String MIMEParams::toString() {
  KJ_IF_SOME(inner, mimeType) {
    return inner.paramsToString();
  }
  return kj::String();
}

jsg::Ref<MIMEParams::EntryIterator> MIMEParams::entries(jsg::Lock& js) {
  kj::Vector<kj::Array<kj::String>> vec;
  KJ_IF_SOME(inner, mimeType) {
    for (const auto& entry: inner.params()) {
      vec.add(kj::arr(kj::str(entry.key), kj::str(entry.value)));
    }
  }
  return js.alloc<EntryIterator>(IteratorState<kj::Array<kj::String>>{vec.releaseAsArray()});
}

jsg::Ref<MIMEParams::KeyIterator> MIMEParams::keys(jsg::Lock& js) {
  kj::Vector<kj::String> vec;
  KJ_IF_SOME(inner, mimeType) {
    for (const auto& entry: inner.params()) {
      vec.add(kj::str(entry.key));
    }
  }
  return js.alloc<KeyIterator>(IteratorState<kj::String>{vec.releaseAsArray()});
}

jsg::Ref<MIMEParams::ValueIterator> MIMEParams::values(jsg::Lock& js) {
  kj::Vector<kj::String> vec;
  KJ_IF_SOME(inner, mimeType) {
    for (const auto& entry: inner.params()) {
      vec.add(kj::str(entry.value));
    }
  }
  return js.alloc<ValueIterator>(IteratorState<kj::String>{vec.releaseAsArray()});
}

MIMEType::MIMEType(jsg::Lock& js, MimeType inner)
    : inner(kj::mv(inner)),
      params(js.alloc<MIMEParams>(this->inner)) {}

MIMEType::~MIMEType() noexcept(false) {
  // Break the connection with the MIMEParams
  params->mimeType = kj::none;
}

jsg::Ref<MIMEType> MIMEType::constructor(jsg::Lock& js, kj::String input) {
  auto parsed =
      JSG_REQUIRE_NONNULL(MimeType::tryParse(input), TypeError, "Not a valid MIME type: ", input);
  return js.alloc<MIMEType>(js, kj::mv(parsed));
}

kj::StringPtr MIMEType::getType() {
  return inner.type();
}

void MIMEType::setType(kj::String type) {
  JSG_REQUIRE(inner.setType(type), TypeError, "Not a valid MIME type");
}

kj::StringPtr MIMEType::getSubtype() {
  return inner.subtype();
}

void MIMEType::setSubtype(kj::String subtype) {
  JSG_REQUIRE(inner.setSubtype(subtype), TypeError, "Not a valid MIME subtype");
}

kj::String MIMEType::getEssence() {
  return inner.essence();
}

jsg::Ref<MIMEParams> MIMEType::getParams() {
  return params.addRef();
}

kj::String MIMEType::toString() {
  return inner.toString();
}

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
