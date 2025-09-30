// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>

namespace workerd::api {

// JavaScript-accessible span class that manages span ownership through IoContext
class JsSpan: public jsg::Object {
 public:
  JsSpan(kj::Maybe<IoOwn<SpanBuilder>> span);
  ~JsSpan() noexcept(false);

  void end();
  // Sets a tag on the span. Values can be string, number, boolean, or undefined.
  // If undefined is passed, the tag is not set (allows optional chaining).
  // Note: We intentionally don't support BigInt/int64_t. JavaScript numbers (doubles)
  // are sufficient for most tracing use cases, and BigInt conversion to int64_t would
  // require handling truncation for values outside the int64_t range.
  void setTag(
      jsg::Lock& js, kj::String key, jsg::Optional<kj::OneOf<bool, double, kj::String>> value);

  JSG_RESOURCE_TYPE(JsSpan) {
    JSG_METHOD(end);
    JSG_METHOD(setTag);
  }

 private:
  kj::Maybe<IoOwn<SpanBuilder>> span;
};

class TracingModule: public jsg::Object {
 public:
  TracingModule() = default;
  TracingModule(jsg::Lock&, const jsg::Url&) {}

  jsg::Ref<JsSpan> startSpan(jsg::Lock& js, const kj::String name);

  JSG_RESOURCE_TYPE(TracingModule) {
    JSG_METHOD(startSpan);

    JSG_NESTED_TYPE(JsSpan);
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

#define EW_TRACING_MODULE_ISOLATE_TYPES api::TracingModule, api::JsSpan
