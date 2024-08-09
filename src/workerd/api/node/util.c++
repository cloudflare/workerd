// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "util.h"
#include <kj/vector.h>
#include <workerd/jsg/modules.h>

namespace workerd::api::node {

MIMEParams::MIMEParams(kj::Maybe<MimeType&> mimeType): mimeType(mimeType) {}

// Oddly, Node.js allows creating MIMEParams directly but it's not actually
// functional. But, to match, we'll go ahead and allow it.
jsg::Ref<MIMEParams> MIMEParams::constructor() {
  return jsg::alloc<MIMEParams>(kj::none);
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
  return kj::str();
}

jsg::Ref<MIMEParams::EntryIterator> MIMEParams::entries(jsg::Lock&) {
  kj::Vector<kj::Array<kj::String>> vec;
  KJ_IF_SOME(inner, mimeType) {
    for (const auto& entry: inner.params()) {
      vec.add(kj::arr(kj::str(entry.key), kj::str(entry.value)));
    }
  }
  return jsg::alloc<EntryIterator>(IteratorState<kj::Array<kj::String>>{vec.releaseAsArray()});
}

jsg::Ref<MIMEParams::KeyIterator> MIMEParams::keys(jsg::Lock&) {
  kj::Vector<kj::String> vec;
  KJ_IF_SOME(inner, mimeType) {
    for (const auto& entry: inner.params()) {
      vec.add(kj::str(entry.key));
    }
  }
  return jsg::alloc<KeyIterator>(IteratorState<kj::String>{vec.releaseAsArray()});
}

jsg::Ref<MIMEParams::ValueIterator> MIMEParams::values(jsg::Lock&) {
  kj::Vector<kj::String> vec;
  KJ_IF_SOME(inner, mimeType) {
    for (const auto& entry: inner.params()) {
      vec.add(kj::str(entry.value));
    }
  }
  return jsg::alloc<ValueIterator>(IteratorState<kj::String>{vec.releaseAsArray()});
}

MIMEType::MIMEType(MimeType inner)
    : inner(kj::mv(inner)),
      params(jsg::alloc<MIMEParams>(this->inner)) {}

MIMEType::~MIMEType() noexcept(false) {
  // Break the connection with the MIMEParams
  params->mimeType = kj::none;
}

jsg::Ref<MIMEType> MIMEType::constructor(kj::String input) {
  auto parsed =
      JSG_REQUIRE_NONNULL(MimeType::tryParse(input), TypeError, "Not a valid MIME type: ", input);
  return jsg::alloc<MIMEType>(kj::mv(parsed));
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

jsg::Optional<UtilModule::PromiseDetails> UtilModule::getPromiseDetails(jsg::JsValue value) {
  auto promise = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsPromise>(), kj::none);
  auto state = promise.state();
  if (state != jsg::PromiseState::PENDING) {
    auto result = promise.result();
    return PromiseDetails{.state = state, .result = result};
  } else {
    return PromiseDetails{.state = state, .result = kj::none};
  }
}

jsg::Optional<UtilModule::ProxyDetails> UtilModule::getProxyDetails(jsg::JsValue value) {
  auto proxy = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsProxy>(), kj::none);
  auto target = proxy.target();
  auto handler = proxy.handler();
  return ProxyDetails{.target = target, .handler = handler};
}

jsg::Optional<UtilModule::PreviewedEntries> UtilModule::previewEntries(jsg::JsValue value) {
  auto object = KJ_UNWRAP_OR_RETURN(value.tryCast<jsg::JsObject>(), kj::none);
  bool isKeyValue;
  auto entries = object.previewEntries(&isKeyValue);
  return PreviewedEntries{.entries = entries, .isKeyValue = isKeyValue};
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

jsg::JsValue UtilModule::getBuiltinModule(jsg::Lock& js, kj::String specifier) {
  auto rawSpecifier = kj::str(specifier);
  bool isNode = false;
  KJ_IF_SOME(spec, jsg::checkNodeSpecifier(specifier)) {
    isNode = true;
    specifier = kj::mv(spec);
  }

  auto registry = jsg::ModuleRegistry::from(js);
  if (registry == nullptr) return js.undefined();
  auto path = kj::Path::parse(specifier);

  KJ_IF_SOME(info,
      registry->resolve(js, path, kj::none, jsg::ModuleRegistry::ResolveOption::BUILTIN_ONLY,
          jsg::ModuleRegistry::ResolveMethod::IMPORT, rawSpecifier.asPtr())) {
    auto module = info.module.getHandle(js);
    jsg::instantiateModule(js, module);
    auto handle = jsg::check(module->Evaluate(js.v8Context()));
    KJ_ASSERT(handle->IsPromise());
    auto prom = handle.As<v8::Promise>();
    KJ_ASSERT(prom->State() != v8::Promise::PromiseState::kPending);
    if (module->GetStatus() == v8::Module::kErrored) {
      jsg::throwTunneledException(js.v8Isolate, module->GetException());
    }

    // For Node.js modules, we want to grab the default export and return that.
    // For other built-ins, we'll return the module namespace instead. Can be
    // a bit confusing but it's a side effect of Node.js modules originally
    // being commonjs and the official getBuiltinModule returning what is
    // expected to be the default export, while the behavior of other built-ins
    // is not really defined by Node.js' implementation.
    if (isNode) {
      return jsg::JsValue(js.v8Get(module->GetModuleNamespace().As<v8::Object>(), "default"_kj));
    } else {
      return jsg::JsValue(module->GetModuleNamespace());
    }
  }

  return js.undefined();
}

}  // namespace workerd::api::node
