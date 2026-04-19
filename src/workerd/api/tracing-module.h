// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {
class TracingModule;  // Forward decl; defined further down after user_tracing::Span.
}  // namespace workerd::api

// The `Span` class exposed to user JavaScript lives in this sub-namespace purely to avoid
// a name collision with the workerd::Span struct that is used internally by the runtime
// tracing implementation. Callers outside this file generally don't need to reference this
// namespace directly - the TracingModule class (which is what gets wired into JS) lives in
// the surrounding workerd::api namespace.
namespace workerd::api::user_tracing {

// The types allowed for tag and log values from JavaScript.
using TagValue = kj::OneOf<bool, double, kj::String>;

// Implementation helper for a tracing span. This is refcounted because the JS-visible Span
// wraps it in either a kj::Own or an IoOwn depending on whether an IoContext is available,
// and because the span may be force-ended while refs are still held by JS code.
//
// SpanImpl owns a reference to the UserTraceSpanHolder that was pushed onto the
// AsyncContextFrame by TracingModule::enterSpan(), and clears that holder's contents when the
// span ends. This ensures that the underlying span observer (which transitively holds a
// kj::Own<BaseTracer> via its SpanSubmitter) is released at end-of-span rather than at
// IoContext destruction. Without this, actor IoContexts would leak tracer references across
// IncomingRequest boundaries and the final "outcome" streaming-tail event would not be emitted
// at the right time.
class SpanImpl final: public kj::Refcounted {
 public:
  // Construct an observed span.
  explicit SpanImpl(kj::Own<workerd::SpanObserver> observer, workerd::Span span);

  // Construct a no-op span (not recording). Used when there is no current user trace span
  // (e.g., running outside a traced request) or when we are in a context where we cannot
  // safely observe spans.
  explicit SpanImpl(decltype(nullptr));

  KJ_DISALLOW_COPY_AND_MOVE(SpanImpl);

  ~SpanImpl() noexcept(false);

  // Records the end time, submits the span via its observer, clears any holder we pushed
  // onto the AsyncContextFrame, and marks the span as no longer traced. Idempotent:
  // subsequent calls are no-ops. Called automatically by TracingModule::enterSpan() on
  // callback completion, and implicitly by the destructor.
  void end();

  bool getIsTraced();

  // Returns a SpanParent wrapping this span's observer, or a null SpanParent if the span has
  // ended or has no observer. Used by TracingModule::enterSpan() to populate the
  // UserTraceSpanHolder that gets pushed onto the AsyncContextFrame.
  workerd::SpanParent makeSpanParent();

  // Sets a single attribute on the span. If value is kj::none, the attribute is not set.
  void setAttribute(kj::String key, kj::Maybe<TagValue> maybeValue);

  // Attach the UserTraceSpanHolder that was pushed onto the AsyncContextFrame when this span
  // was activated. Called by TracingModule::enterSpan() before running the callback. When the
  // span ends, we clear() the holder so that the AsyncContextFrame's reference no longer
  // keeps the span observer (and thus the BaseTracer) alive.
  void attachAsyncContextHolder(kj::Own<workerd::UserTraceSpanHolder> holder);

 private:
  kj::Maybe<kj::Own<workerd::SpanObserver>> observer;

  // The under-construction span, or kj::none if the span has ended.
  kj::Maybe<workerd::Span> span;

  // The AsyncContextFrame-pushed holder for this span, if any. Cleared in end() to release
  // the underlying SpanParent (and the observer + BaseTracer chain) promptly.
  kj::Maybe<kj::Own<workerd::UserTraceSpanHolder>> asyncContextHolder;

  size_t bytesUsed = 0;

  void setSpanDataLimitError(kj::StringPtr itemKind, kj::StringPtr name, size_t valueSize);
};

// JavaScript-accessible tracing span (exposed as `Span`). From the user's perspective this
// is the only kind of span there is; internal C++ plumbing lives on SpanImpl. Kept in the
// workerd::api::user_tracing namespace (not workerd::api) to avoid collision with the
// runtime's own workerd::Span type.
//
// The impl is wrapped in IoOwn when an IoContext exists, so that destruction is funneled
// through the IoContext's delete queue and cannot cross threads. When no IoContext is
// available (unusual for user tracing - typically startup paths), a plain kj::Own is used.
class Span: public jsg::Object {
 public:
  explicit Span(kj::OneOf<kj::Own<SpanImpl>, IoOwn<SpanImpl>> impl);

  // Returns true if this span will be recorded. False when the current async context is not
  // being traced, or when the span has already been submitted (which happens automatically
  // when the enterSpan callback returns). Callers can gate expensive attribute-computation
  // code on this.
  bool getIsTraced();

  // Sets a single attribute. If `value` is undefined, the attribute is not set - useful for
  // optional fields.
  void setAttribute(jsg::Lock& js, kj::String key, jsg::Optional<TagValue> value);

  // Ends the span and submits its content to the tracing system. Not exposed to JS - only
  // called by TracingModule::enterSpan when the user callback returns / throws / its promise
  // settles. Callers outside this file should not need it.
  void end();

  JSG_RESOURCE_TYPE(Span) {
    JSG_READONLY_PROTOTYPE_PROPERTY(isTraced, getIsTraced);

    JSG_METHOD(setAttribute);
  }

 private:
  kj::OneOf<kj::Own<SpanImpl>, IoOwn<SpanImpl>> impl;

  friend class ::workerd::api::TracingModule;
};

}  // namespace workerd::api::user_tracing

namespace workerd::api {

// Module providing user tracing capabilities to Workers. Exposed as
// "cloudflare-internal:tracing".
class TracingModule: public jsg::Object {
 public:
  TracingModule() = default;
  TracingModule(jsg::Lock&, const jsg::Url&) {}

  // Creates a new child span of the current user trace span, pushes it onto the
  // AsyncContextFrame as the active user span, invokes callback(span, ...args), and
  // automatically ends the span on completion. If the callback returns a Promise, the
  // span is ended when the promise settles (whether fulfilled or rejected). If the
  // callback returns synchronously (or throws synchronously), the span is ended
  // before the return (or rethrow).
  //
  // The span is constructed as a child of whatever span is currently active on the
  // AsyncContextFrame (or, if none, the root user request span on the current
  // IncomingRequest, via IoContext::getCurrentUserTraceSpan()).
  //
  // If no IoContext is available (e.g., during worker startup), the callback runs with
  // a no-op span and no AsyncContextFrame push.
  v8::Local<v8::Value> enterSpan(jsg::Lock& js,
      kj::String operationName,
      v8::Local<v8::Function> callback,
      jsg::Arguments<jsg::Value> args,
      const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler,
      const jsg::TypeHandler<jsg::Promise<jsg::Value>>& valuePromiseHandler);

  JSG_RESOURCE_TYPE(TracingModule) {
    JSG_METHOD(enterSpan);

    // Use the _NAMED variant so the property ends up as `tracing.Span` rather than
    // `tracing["user_tracing::Span"]`.
    JSG_NESTED_TYPE_NAMED(user_tracing::Span, Span);
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

}  // namespace workerd::api

#define EW_TRACING_MODULE_ISOLATE_TYPES api::TracingModule, api::user_tracing::Span
