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
  return kj::String();
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

    objects.add(CallSiteEntry{
      .functionName = js.toString(stack_frame->GetFunctionName()),
      .scriptName = js.toString(stack_frame->GetScriptName()),
      .lineNumber = stack_frame->GetLineNumber(),
      // Node.js originally implemented the experimental API using the "column" field
      // then later renamed it to columnNumber. We had already implemented the API
      // using column. To ensure backwards compat without the complexity of a compat
      // flag, we just export both.
      .columnNumber = stack_frame->GetColumn(),
      .column = stack_frame->GetColumn(),
    });
  }

  return objects.releaseAsArray();
}

jsg::JsValue UtilModule::getBuiltinModule(jsg::Lock& js, kj::String specifier) {
  auto rawSpecifier = kj::str(specifier);
  bool isNode = false;
  KJ_IF_SOME(spec, jsg::checkNodeSpecifier(specifier)) {
    isNode = true;
    specifier = kj::mv(spec);
  }

  if (FeatureFlags::get(js).getNewModuleRegistry()) {
    KJ_IF_SOME(mod, js.resolveInternalModule(specifier)) {
      return mod;
    }
    return js.undefined();
  }

  auto registry = jsg::ModuleRegistry::from(js);
  if (registry == nullptr) return js.undefined();
  auto path = kj::Path::parse(specifier);

  KJ_IF_SOME(info,
      registry->resolve(js, path, kj::none, jsg::ModuleRegistry::ResolveOption::BUILTIN_ONLY,
          jsg::ModuleRegistry::ResolveMethod::IMPORT, rawSpecifier.asPtr())) {
    auto module = info.module.getHandle(js);
    jsg::instantiateModule(js, module);

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

jsg::JsObject UtilModule::getEnvObject(jsg::Lock& js) {
  if (FeatureFlags::get(js).getPopulateProcessEnv()) {
    KJ_IF_SOME(env, js.getWorkerEnv()) {
      return jsg::JsObject(env.getHandle(js));
    }
  }

  // Default to empty object.
  return js.obj();
}

namespace {
[[noreturn]] void handleProcessExit(jsg::Lock& js, int code) {
  // There are a few things happening here. First, we abort the current IoContext
  // in order to shut down this specific request....
  auto message =
      kj::str("The Node.js process.exit(", code, ") API was called. Canceling the request.");
  auto& ioContext = IoContext::current();
  // If we have a tail worker, let's report the error.
  KJ_IF_SOME(tracer, ioContext.getWorkerTracer()) {
    // Why create the error like this in tracing? Because we're adding the exception
    // to the trace and ideally we'd have the JS stack attached to it. Just using
    // JSG_KJ_EXCEPTION would not give us that, and we only want to incur the cost
    // of creating and capturing the stack when we actually need it.
    auto ex = KJ_ASSERT_NONNULL(js.error(message).tryCast<jsg::JsObject>());
    tracer.addException(ioContext.getInvocationSpanContext(), ioContext.now(),
        ex.get(js, "name"_kj).toString(js), ex.get(js, "message"_kj).toString(js),
        ex.get(js, "stack"_kj).toString(js));
    ioContext.abort(js.exceptionToKj(ex));
  } else {
    ioContext.abort(JSG_KJ_EXCEPTION(FAILED, Error, kj::mv(message)));
  }
  // ...then we tell the isolate to terminate the current JavaScript execution.
  // Oddly however, this does not appear to *actually* terminate the thread of
  // execution unless we trigger the Isolate to handle the intercepts, which
  // calling v8::JSON::Stringify does. Weird... but ok? As long as it works
  // TODO(soon): Investigate if there is a better approach to triggering the
  // interrupt handling.
  js.v8Isolate->TerminateExecution();
  jsg::check(v8::JSON::Stringify(js.v8Context(), js.str()));
  // This should be unreachable here as we expect the isolate to terminate and
  // an exception to have been thrown.
  KJ_UNREACHABLE;
}
}  // namespace

void UtilModule::processExitImpl(jsg::Lock& js, int code) {
  if (IoContext::hasCurrent()) {
    handleProcessExit(js, code);
  }

  // Create an error object so we can easily capture the stack where the
  // process.exit call was made.
  auto err = KJ_ASSERT_NONNULL(
      js.error("process.exit(...) called without a current request context. Ignoring.")
          .tryCast<jsg::JsObject>());
  err.set(js, "name"_kj, js.str());
  js.logWarning(kj::str(err.get(js, "stack"_kj)));
}

}  // namespace workerd::api::node
