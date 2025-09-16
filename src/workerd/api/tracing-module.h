// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "http.h"
#include "worker-rpc.h"

#include <workerd/io/io-context.h>

namespace workerd::api {

// Forward declaration for InternalSpanImpl
class InternalSpanImpl;

// Wrapper class that manages span ownership through IoContext
class InternalSpan: public jsg::Object {
 public:
  InternalSpan(kj::Maybe<IoOwn<InternalSpanImpl>> impl);

  void end();
  void setTag(jsg::Lock& js, kj::String key, kj::Maybe<kj::OneOf<bool, double, kj::String>> value);
  bool getIsRecording();
  SpanParent makeSpanParent();

  JSG_RESOURCE_TYPE(InternalSpan) {
    JSG_METHOD(end);
    JSG_METHOD(setTag);
    JSG_METHOD(getIsRecording);
  }

  friend class InternalSpanImpl;

 private:
  kj::Maybe<IoOwn<InternalSpanImpl>> impl;
};

// Implementation class that actually manages the SpanBuilder
class InternalSpanImpl {
 public:
  InternalSpanImpl(SpanBuilder span);
  ~InternalSpanImpl() noexcept(false);

  void end();
  void setTag(kj::ConstString key, kj::OneOf<bool, double, kj::String> value);
  bool getIsRecording();
  SpanParent makeSpanParent();

 private:
  kj::Maybe<SpanBuilder> span;
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
