// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "http.h"
#include "worker-rpc.h"

namespace workerd::api {

class JsSpanBuilder: public jsg::Object {
 private:
  SpanBuilder span;

 public:
  JsSpanBuilder(SpanBuilder span): span(kj::mv(span)) {}

  void end() {
    span.end();
  }

  void setTag(jsg::Lock& js, const kj::String key, const jsg::Value& value);

  JSG_RESOURCE_TYPE(JsSpanBuilder) {
    JSG_METHOD(end);
    JSG_METHOD(setTag);
  }
};

class TracingModule: public jsg::Object {
 public:
  TracingModule() = default;
  TracingModule(jsg::Lock&, const jsg::Url&) {}

  jsg::Ref<JsSpanBuilder> startSpan(jsg::Lock& js, const kj::String name);

  JSG_RESOURCE_TYPE(TracingModule) {
    JSG_METHOD(startSpan);

    JSG_NESTED_TYPE(JsSpanBuilder);
  }
};

#define EW_TRACING_MODULE_ISOLATE_TYPES api::TracingModule, api::JsSpanBuilder

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
