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

  // Ends the span, marking its completion. Once ended, the span cannot be modified.
  // If the span is not explicitly ended, it will be automatically ended when the
  // JsSpan object is destroyed.
  void end();
  // Sets an attribute on the span. Values can be string, number, boolean, or undefined.
  // If undefined is passed, the attribute is not set (allows optional chaining).
  // Note: We intentionally don't support BigInt/int64_t. JavaScript numbers (doubles)
  // are sufficient for most tracing use cases, and BigInt conversion to int64_t would
  // require handling truncation for values outside the int64_t range.
  void setAttribute(
      jsg::Lock& js, kj::String key, jsg::Optional<kj::OneOf<bool, double, kj::String>> value);

  JSG_RESOURCE_TYPE(JsSpan) {
    JSG_METHOD(end);
    JSG_METHOD(setAttribute);
  }

 private:
  kj::Maybe<IoOwn<SpanBuilder>> span;
};

// Module that provides tracing capabilities for Workers.
// This module is available as "cloudflare-internal:tracing" and provides
// functionality to create and manage tracing spans.
class TracingModule: public jsg::Object {
 public:
  TracingModule() = default;
  TracingModule(jsg::Lock&, const jsg::Url&) {}

  // Creates a new tracing span with the given name.
  // The span will be associated with the current IoContext and will track
  // the execution of the code within its lifetime.
  // If no IoContext is available (e.g., during initialization), a no-op span
  // is returned that safely ignores all operations.
  //
  // Example usage:
  //   const span = tracing.startSpan("my-operation");
  //   try {
  //     // ... perform operation ...
  //   } finally {
  //     span.end();
  //   }
  jsg::Ref<JsSpan> startSpan(jsg::Lock& js, kj::String name);

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
