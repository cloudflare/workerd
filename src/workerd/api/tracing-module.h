// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "http.h"
#include "worker-rpc.h"

#include <workerd/io/io-context.h>

namespace workerd::api {

// Wrapper class that manages span ownership through IoContext
class InternalSpan: public jsg::Object {
 public:
  InternalSpan(kj::Maybe<IoOwn<SpanBuilder>> span);
  ~InternalSpan() noexcept(false);

  void end();
  void setTag(
      jsg::Lock& js, kj::String key, jsg::Optional<kj::OneOf<bool, double, kj::String>> value);
  bool getIsRecording();
  SpanParent makeSpanParent();

  JSG_RESOURCE_TYPE(InternalSpan) {
    JSG_METHOD(end);
    JSG_METHOD(setTag);
    JSG_METHOD(getIsRecording);
  }

 private:
  kj::Maybe<IoOwn<SpanBuilder>> span;
};

// Keep JsSpanBuilder as alias for backward compatibility
using JsSpanBuilder = InternalSpan;

class TracingModule: public jsg::Object {
 public:
  TracingModule() = default;
  TracingModule(jsg::Lock&, const jsg::Url&) {}

  jsg::Ref<InternalSpan> startSpan(jsg::Lock& js, const kj::String name);
  jsg::JsValue startSpanWithCallback(jsg::Lock& js,
      kj::String operationName,
      jsg::Function<jsg::Value(jsg::Arguments<jsg::Value>)> callback,
      jsg::Arguments<jsg::Value> args,
      const jsg::TypeHandler<jsg::Ref<InternalSpan>>& jsSpanHandler,
      const jsg::TypeHandler<jsg::Promise<jsg::Value>>& valuePromiseHandler);

  JSG_RESOURCE_TYPE(TracingModule) {
    JSG_METHOD(startSpan);
    JSG_METHOD(startSpanWithCallback);

    JSG_NESTED_TYPE(InternalSpan);
  }
};

template <class Registry>
void registerTracingModule(Registry& registry, CompatibilityFlags::Reader flags) {
  registry.template addBuiltinModule<TracingModule>(
      "cloudflare-internal:tracing", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalTracingModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "cloudflare-internal:tracing"_url;
  builder.addObject<TracingModule, TypeWrapper>(kSpecifier);
  return builder.finish();
}
};  // namespace workerd::api

#define EW_TRACING_MODULE_ISOLATE_TYPES api::TracingModule, api::InternalSpan
