// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracing.h"

#include <workerd/io/trace.h>
#include <workerd/io/tracer.h>
#include <workerd/util/thread-scopes.h>

namespace workerd::api::user_tracing {

namespace {

// Approximately how much data we allow to be added to a user span before we start ignoring
// modification requests. This is a soft cap to prevent accidental misuse from unbounded
// memory growth; downstream tail-stream submission may apply additional limits.
constexpr size_t MAX_SPAN_BYTES = 64 * 1024;

size_t estimateTagValueSize(TagValue& value) {
  // Approximate size; different encodings will produce different sizes. The goal is to bound
  // accidental overuse, not to be byte-accurate.
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(b, bool) {
      return 8;
    }
    KJ_CASE_ONEOF(d, double) {
      return 8;
    }
    KJ_CASE_ONEOF(s, kj::String) {
      return s.size();
    }
  }
  KJ_UNREACHABLE;
}

// This is a CF semantic for warning conditions surfaced on spans, modeled on OpenTelemetry's exception
// semantic conventions (`exception.type` / `exception.message`).
enum class SpanWarningType {
  SPAN_DATA_LIMIT_EXCEEDED,
};

kj::LiteralStringConst spanWarningTypeName(SpanWarningType type) {
  switch (type) {
    case SpanWarningType::SPAN_DATA_LIMIT_EXCEEDED:
      return "span_data_limit_exceeded"_kjc;
  }
  KJ_UNREACHABLE;
}

}  // namespace

// ======================================================================================
// SpanState

void SpanState::setAttribute(kj::String key, kj::Maybe<TagValue> maybeValue) {
  if (!canRecordAttributes()) {
    return;
  }
  KJ_IF_SOME(value, maybeValue) {
    if (bytesUsed > MAX_SPAN_BYTES) {
      return;
    }
    size_t valueSize = estimateTagValueSize(value);
    bytesUsed += key.size() + valueSize;
    if (bytesUsed > MAX_SPAN_BYTES) {
      recordSpanDataLimitError("attribute", key, valueSize);
      return;
    }
    recordAttribute(kj::mv(key), kj::mv(value));
  }
  // If value is kj::none the attribute is left unset (undefined on the JS side).
}

void SpanState::recordException(
    kj::String name, kj::String message, kj::Maybe<kj::String> stack) {
  if (!canRecordAttributes()) {
    return;
  }

  size_t valueSize = name.size() + message.size();
  KJ_IF_SOME(s, stack) {
    valueSize += s.size();
  }
  bytesUsed += valueSize;
  if (bytesUsed > MAX_SPAN_BYTES) {
    recordSpanDataLimitError("exception", name, valueSize);
    return;
  }
  recordExceptionImpl(kj::mv(name), kj::mv(message), kj::mv(stack));
}

class UserSpanState final: public SpanState {
 public:
  UserSpanState(kj::Rc<workerd::SpanObserver> observer, kj::ConstString operationName)
      : builder(kj::mv(observer), kj::mv(operationName)) {}

  ~UserSpanState() noexcept(false) override {
    end();
  }

  void end() override {
    // Move-assigning a null builder ends the old one (submitting via onClose) and drops the
    // observer reference so subsequent setTag/isObserved calls no-op.
    builder = workerd::SpanBuilder(nullptr);
  }

  bool getIsTraced() override {
    return builder.isObserved();
  }

  workerd::SpanParent makeSpanParent() override {
    return workerd::SpanParent(builder);
  }

 protected:
  bool canRecordAttributes() override {
    return builder.isObserved();
  }

  void recordAttribute(kj::String key, TagValue value) override {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(b, bool) {
        builder.setTag(kj::ConstString(kj::mv(key)), b, IsCustomTag::YES);
      }
      KJ_CASE_ONEOF(d, double) {
        builder.setTag(kj::ConstString(kj::mv(key)), d, IsCustomTag::YES);
      }
      KJ_CASE_ONEOF(s, kj::String) {
        builder.setTag(kj::ConstString(kj::mv(key)), kj::mv(s), IsCustomTag::YES);
      }
    }
  }

  void recordExceptionImpl(
      kj::String name, kj::String message, kj::Maybe<kj::String> stack) override {
    builder.recordException(kj::mv(name), kj::mv(message), kj::mv(stack));
  }

  void recordSpanDataLimitError(
      kj::StringPtr itemKind, kj::StringPtr name, size_t valueSize) override {
    if (!builder.isObserved()) {
      return;
    }
    kj::String shortName;
    if (name.size() > 64) {
      shortName = kj::str("\"", name.slice(0, 64), "...\" (key length ", name.size(), ")");
    } else {
      shortName = kj::str("\"", name, "\"");
    }
    auto message = kj::ConstString(kj::str("exceeded span data limit while trying to record ",
        itemKind, " ", shortName, " of size ", valueSize));
    builder.setTag("cloudflare.warning.type"_kjc,
        spanWarningTypeName(SpanWarningType::SPAN_DATA_LIMIT_EXCEEDED));
    builder.setTag("cloudflare.warning.message"_kjc, kj::mv(message));
  }

 private:
  workerd::SpanBuilder builder;
};

class NoopSpanState final: public SpanState {
 public:
  void end() override {}

  bool getIsTraced() override {
    return false;
  }

  workerd::SpanParent makeSpanParent() override {
    return workerd::SpanParent(nullptr);
  }

 protected:
  bool canRecordAttributes() override {
    return false;
  }

  void recordAttribute(kj::String, TagValue) override {}

  void recordExceptionImpl(kj::String, kj::String, kj::Maybe<kj::String>) override {}
};

// ======================================================================================
// Span

Span::Span(kj::OneOf<kj::Own<SpanState>, IoOwn<SpanState>> state): state(kj::mv(state)) {}

bool Span::getIsTraced() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      return s->getIsTraced();
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      return s->getIsTraced();
    }
  }
  KJ_UNREACHABLE;
}

jsg::Ref<Span> Span::setAttribute(jsg::Lock& js, kj::String key, jsg::Optional<TagValue> value) {
  kj::Maybe<TagValue> maybeValue;
  KJ_IF_SOME(v, value) {
    maybeValue = kj::mv(v);
  }
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      s->setAttribute(kj::mv(key), kj::mv(maybeValue));
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      s->setAttribute(kj::mv(key), kj::mv(maybeValue));
    }
  }
  return JSG_THIS;
}

jsg::Ref<Span> Span::setAttributes(jsg::Lock& js, jsg::Dict<jsg::Optional<TagValue>> attributes) {
  for (auto& field: attributes.fields) {
    setAttribute(js, kj::mv(field.name), kj::mv(field.value));
  }
  return JSG_THIS;
}

void Span::recordException(
    jsg::Lock& js, jsg::Value exception, const jsg::TypeHandler<ExceptionData>& exceptionHandler) {
  if (!getIsTraced()) {
    return;
  }

  kj::String name;
  kj::String message;
  kj::Maybe<kj::String> stack;
  auto handle = exception.getHandle(js);
  if (handle->IsString()) {
    message = jsg::JsValue(handle).toString(js);
  } else if (handle->IsObject()) {
    auto data = KJ_REQUIRE_NONNULL(exceptionHandler.tryUnwrap(js, handle));
    KJ_IF_SOME(code, data.code) {
      KJ_SWITCH_ONEOF(code) {
        KJ_CASE_ONEOF(s, kj::String) {
          if (s.size() > 0) {
            name = kj::mv(s);
          }
        }
        KJ_CASE_ONEOF(n, double) {
          if (n != 0 && n == n) {
            name = kj::str(n);
          }
        }
      }
    }
    if (name.size() == 0) {
      KJ_IF_SOME(n, data.name) {
        name = kj::mv(n);
      }
    }
    KJ_IF_SOME(m, data.message) {
      message = kj::mv(m);
    }
    KJ_IF_SOME(s, data.stack) {
      stack = kj::mv(s);
    }

    if (name.size() == 0 && message.size() == 0) {
      return;
    }
  } else {
    return;
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      s->recordException(kj::mv(name), kj::mv(message), kj::mv(stack));
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      s->recordException(kj::mv(name), kj::mv(message), kj::mv(stack));
    }
  }
}

void Span::end() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      s->end();
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      s->end();
    }
  }
}

}  // namespace workerd::api::user_tracing

// ======================================================================================
// Tracing

namespace workerd::api {

namespace {

enum class SpanEndMode { AUTO_END, MANUAL_END };

struct CreatedSpan {
  jsg::Ref<user_tracing::Span> span;
  kj::Maybe<SpanParent> childSpanForAsyncContext;
};

CreatedSpan createSpan(jsg::Lock& js, kj::String operationName) {
  // We use qualified `user_tracing::Span` / `user_tracing::SpanState` throughout because an
  // unqualified `Span` in this namespace resolves to workerd::Span (the runtime span struct),
  // which is a different type.

  // Cap operation name length at the API boundary so every downstream submitter sees the
  // truncated value.
  if (operationName.size() > user_tracing::MAX_USER_OPERATION_NAME_BYTES) {
    operationName = kj::str(operationName.first(user_tracing::MAX_USER_OPERATION_NAME_BYTES));
  }

  kj::Own<user_tracing::SpanState> state;
  kj::Maybe<SpanParent> childSpanForAsyncContext;
  bool hasIoContext = IoContext::hasCurrent();

  if (hasIoContext) {
    auto& context = IoContext::current();
    SpanParent parent = context.getCurrentUserTraceSpan();

    if (parent.isObserved()) {
      KJ_IF_SOME(observer, parent.getObserver()) {
        // newChildFromUserCode (vs newChild) signals user-origin to the submitter so it can
        // skip the operation-name allowlist that gates runtime spans.
        auto childObserver = observer.newChildFromUserCode();
        state = kj::refcounted<user_tracing::UserSpanState>(
            kj::mv(childObserver), kj::ConstString(kj::heapString(operationName)));
        // Capture a SpanParent for the child so startActiveSpan() / enterSpan() can push it onto
        // the AsyncContextFrame. Safe to carry across the request boundary thanks to
        // BaseTracer::WeakRef in the submitter - stale parents cannot pin the tracer.
        childSpanForAsyncContext = state->makeSpanParent();
      } else {
        state = kj::refcounted<user_tracing::NoopSpanState>();
      }
    } else {
      state = kj::refcounted<user_tracing::NoopSpanState>();
    }
  } else {
    // No IoContext: create a no-op span.
    state = kj::refcounted<user_tracing::NoopSpanState>();
  }

  // Wrap state in IoOwn (when inside an IoContext) so destruction funnels through the
  // IoContext's delete queue and cannot cross threads. Outside an IoContext, fall back to
  // kj::Own; tracing without an IoContext is a no-op tracing-wise.
  auto span = [&]() -> jsg::Ref<user_tracing::Span> {
    if (hasIoContext) {
      auto ownedState = IoContext::current().addObject(kj::mv(state));
      return js.alloc<user_tracing::Span>(kj::mv(ownedState));
    }
    return js.alloc<user_tracing::Span>(kj::mv(state));
  }();

  return CreatedSpan{
    .span = kj::mv(span), .childSpanForAsyncContext = kj::mv(childSpanForAsyncContext)};
}

v8::Local<v8::Value> runSpan(jsg::Lock& js,
    kj::String operationName,
    v8::Local<v8::Function> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::Value>>* valuePromiseHandler,
    SpanEndMode endMode) {
  auto createdSpan = createSpan(js, kj::mv(operationName));
  auto jsSpan = kj::mv(createdSpan.span);

  // Build argv for the callback: (span, ...args).
  v8::LocalVector<v8::Value> argv(js.v8Isolate);
  argv.push_back(spanHandler.wrap(js, jsSpan.addRef()));
  for (auto& arg: args) {
    argv.push_back(arg.getHandle(js));
  }

  auto executeCallback = [&]() -> v8::Local<v8::Value> {
    auto v8Context = js.v8Context();
    return js.tryCatch([&]() -> v8::Local<v8::Value> {
      auto result =
          jsg::check(callback->Call(v8Context, v8Context->Global(), argv.size(), argv.data()));

      if (endMode == SpanEndMode::MANUAL_END) {
        return result;
      }

      // If the callback returned a promise, defer end() until settlement.
      if (result->IsPromise()) {
        KJ_ASSERT(valuePromiseHandler != nullptr);
        auto promise = KJ_ASSERT_NONNULL(valuePromiseHandler->tryUnwrap(js, result))
                           .then(js,
                               [jsSpan = jsSpan.addRef()](
                                   jsg::Lock& js, jsg::Value value) mutable -> jsg::Value {
          jsSpan->end();
          return kj::mv(value);
        },
                               [jsSpan = jsSpan.addRef()](
                                   jsg::Lock& js, jsg::Value exception) mutable -> jsg::Value {
          jsSpan->end();
          js.throwException(kj::mv(exception));
        });
        // If the promise never settles, the span will still be submitted when the IoOwn is
        // destroyed (via ~SpanState calling end()), though this is a corner case and should
        // generally be avoided by users.
        return valuePromiseHandler->wrap(js, kj::mv(promise));
      } else {
        // Synchronous success: end immediately.
        jsSpan->end();
        return result;
      }
    }, [&](jsg::Value exception) -> v8::Local<v8::Value> {
      if (endMode == SpanEndMode::AUTO_END) {
        // Synchronous exception: end then rethrow.
        jsSpan->end();
      }
      js.throwException(kj::mv(exception));
    });
  };

  // If we have an IoContext and an observed child span, push it onto the AsyncContextFrame
  // for the duration of the callback. The StorageScope RAII object restores the prior
  // async-context storage on scope exit; any async continuations captured during the
  // callback will already have snapshotted the new frame and will see our child span as
  // "current".
  KJ_IF_SOME(span, kj::mv(createdSpan.childSpanForAsyncContext)) {
    auto& context = IoContext::current();
    jsg::AsyncContextFrame::StorageScope traceScope =
        context.makeUserAsyncTraceScope(context.getCurrentLock(), kj::mv(span));
    return executeCallback();
  } else {
    return executeCallback();
  }
}

}  // namespace

v8::Local<v8::Value> Tracing::enterSpan(jsg::Lock& js,
    kj::String operationName,
    v8::Local<v8::Function> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::Value>>& valuePromiseHandler) {
  return runSpan(js, kj::mv(operationName), callback, kj::mv(args), spanHandler,
      &valuePromiseHandler, SpanEndMode::AUTO_END);
}

v8::Local<v8::Value> Tracing::startActiveSpan(jsg::Lock& js,
    kj::String operationName,
    v8::Local<v8::Function> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler) {
  return runSpan(js, kj::mv(operationName), callback, kj::mv(args), spanHandler, nullptr,
      SpanEndMode::MANUAL_END);
}

jsg::Ref<user_tracing::Span> Tracing::startSpan(jsg::Lock& js, kj::String operationName) {
  return createSpan(js, kj::mv(operationName)).span;
}

}  // namespace workerd::api
