// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {
class Tracing;  // Forward decl; defined further down after user_tracing::Span.
}  // namespace workerd::api

// The `Span` class exposed to user JavaScript lives in this sub-namespace purely to avoid
// a name collision with the workerd::Span struct that is used internally by the runtime
// tracing implementation. Callers outside this file generally don't need to reference this
// namespace directly - the Tracing class (which is what gets wired into JS) lives in
// the surrounding workerd::api namespace.
namespace workerd::api::user_tracing {

// Max length of a user-supplied operation name in `ctx.tracing.enterSpan(name, ...)`.
// Longer names are truncated at the API surface so the limit holds for every downstream
// SpanSubmitter. Span names identify operations, not carry data; the bound is tight on
// purpose.
constexpr size_t MAX_USER_OPERATION_NAME_BYTES = 64;

// The types allowed for tag and log values from JavaScript.
using TagValue = kj::OneOf<bool, double, kj::String>;

// Refcounted wrapper around workerd::SpanBuilder, exposing the JS Span surface: bytes-used
// limit enforcement and JS-side TagValue forwarding. Span lifecycle (onOpen/onClose) is
// delegated to SpanBuilder.
class SpanImpl final: public kj::Refcounted {
 public:
  // Construct an observed span. The builder drives the observer's onOpen immediately.
  SpanImpl(kj::Own<workerd::SpanObserver> observer, kj::ConstString operationName);

  // Construct a no-op span (not recording). Used when there is no current user trace span
  // (e.g., running outside a traced request) or when we are in a context where we cannot
  // safely observe spans.
  explicit SpanImpl(decltype(nullptr));

  KJ_DISALLOW_COPY_AND_MOVE(SpanImpl);

  ~SpanImpl() noexcept(false);

  // Submits the span and marks it as no longer traced. Idempotent; the destructor calls
  // end() as well.
  void end();

  bool getIsTraced();

  // Returns a SpanParent wrapping this span's observer, or a null SpanParent if the span has
  // ended or has no observer. Used by Tracing::enterSpan() to push onto the AsyncContextFrame.
  workerd::SpanParent makeSpanParent();

  // Sets a single attribute on the span. If value is kj::none, the attribute is not set.
  void setAttribute(kj::String key, kj::Maybe<TagValue> maybeValue);

 private:
  workerd::SpanBuilder builder;

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
  // called by Tracing::enterSpan when the user callback returns / throws / its promise
  // settles. Callers outside this file should not need it.
  void end();

  JSG_RESOURCE_TYPE(Span) {
    JSG_READONLY_PROTOTYPE_PROPERTY(isTraced, getIsTraced);

    JSG_METHOD(setAttribute);
  }

 private:
  kj::OneOf<kj::Own<SpanImpl>, IoOwn<SpanImpl>> impl;

  friend class ::workerd::api::Tracing;
};

}  // namespace workerd::api::user_tracing

namespace workerd::api {

// User-tracing module. Exposed to JS as the `Tracing` class, reachable both via
// `import { tracing } from 'cloudflare:workers'` and as `ctx.tracing`, and registered as
// the builtin module `cloudflare-internal:tracing`. The class name (not "TracingModule")
// is what shows up in `.d.ts` output and in `typeof ctx.tracing` — historically this was
// called `TracingModule` back when the API was only reachable via an ES module import;
// that suffix is vestigial now that it's also a property on `ctx`.
class Tracing: public jsg::Object {
 public:
  Tracing() = default;
  Tracing(jsg::Lock&, const jsg::Url&) {}

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

  JSG_RESOURCE_TYPE(Tracing) {
    JSG_METHOD(enterSpan);

    // Use the _NAMED variant so the property ends up as `tracing.Span` rather than
    // `tracing["user_tracing::Span"]`.
    JSG_NESTED_TYPE_NAMED(user_tracing::Span, Span);

    // Override the auto-generated `enterSpan(name: string, callback: Function, ...args:
    // any[]): any` with a properly-typed generic form: the callback's first argument is
    // typed as `Span`, the callback's trailing args flow through to the varargs, and the
    // return value is preserved. Matches the shape documented in the user-tracing RFC.
    JSG_TS_OVERRIDE({
      enterSpan<T, A extends unknown[]>(
        name: string,
        callback: (span: Span, ...args: A) => T,
        ...args: A
      ): T;
    });
  }
};

// Registers `cloudflare-internal:tracing` as a builtin module. The `Module` suffix on the
// helper is describing what it registers (a JS module), not the name of the class — the
// class itself is `Tracing` because that's what users see.
template <class Registry>
void registerTracingModule(Registry& registry, CompatibilityFlags::Reader flags) {
  registry.template addBuiltinModule<Tracing>(
      "cloudflare-internal:tracing", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalTracingModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "cloudflare-internal:tracing"_url;
  builder.addObject<Tracing, TypeWrapper>(kSpecifier);
  return builder.finish();
}

}  // namespace workerd::api

#define EW_TRACING_ISOLATE_TYPES api::Tracing, api::user_tracing::Span
