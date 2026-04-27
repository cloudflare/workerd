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

}  // namespace

// ======================================================================================
// SpanImpl

SpanImpl::SpanImpl(kj::Own<workerd::SpanObserver> observer, kj::ConstString operationName)
    : builder(kj::mv(observer), kj::mv(operationName)) {}

SpanImpl::SpanImpl(decltype(nullptr)): builder(nullptr) {}

SpanImpl::~SpanImpl() noexcept(false) {
  end();
}

void SpanImpl::end() {
  // Move-assigning a null builder ends the old one (submitting via onClose) and drops the
  // observer reference so subsequent setTag/isObserved calls no-op.
  builder = workerd::SpanBuilder(nullptr);
}

bool SpanImpl::getIsTraced() {
  return builder.isObserved();
}

workerd::SpanParent SpanImpl::makeSpanParent() {
  return workerd::SpanParent(builder);
}

void SpanImpl::setAttribute(kj::String key, kj::Maybe<TagValue> maybeValue) {
  if (!builder.isObserved()) {
    return;
  }
  KJ_IF_SOME(value, maybeValue) {
    if (bytesUsed > MAX_SPAN_BYTES) {
      return;
    }
    size_t valueSize = estimateTagValueSize(value);
    bytesUsed += key.size() + valueSize;
    if (bytesUsed > MAX_SPAN_BYTES) {
      setSpanDataLimitError("attribute", key, valueSize);
      return;
    }
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(b, bool) {
        builder.setTag(kj::ConstString(kj::mv(key)), b);
      }
      KJ_CASE_ONEOF(d, double) {
        builder.setTag(kj::ConstString(kj::mv(key)), d);
      }
      KJ_CASE_ONEOF(s, kj::String) {
        builder.setTag(kj::ConstString(kj::mv(key)), kj::mv(s));
      }
    }
  }
  // If value is kj::none the attribute is left unset (undefined on the JS side).
}

void SpanImpl::setSpanDataLimitError(kj::StringPtr itemKind, kj::StringPtr name, size_t valueSize) {
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
  builder.setTag("span_error"_kjc, kj::mv(message));
}

// ======================================================================================
// Span

Span::Span(kj::OneOf<kj::Own<SpanImpl>, IoOwn<SpanImpl>> impl): impl(kj::mv(impl)) {}

bool Span::getIsTraced() {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(s, kj::Own<SpanImpl>) {
      return s->getIsTraced();
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanImpl>) {
      return s->getIsTraced();
    }
  }
  KJ_UNREACHABLE;
}

void Span::setAttribute(jsg::Lock& js, kj::String key, jsg::Optional<TagValue> value) {
  kj::Maybe<TagValue> maybeValue;
  KJ_IF_SOME(v, value) {
    maybeValue = kj::mv(v);
  }
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(s, kj::Own<SpanImpl>) {
      s->setAttribute(kj::mv(key), kj::mv(maybeValue));
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanImpl>) {
      s->setAttribute(kj::mv(key), kj::mv(maybeValue));
    }
  }
}

void Span::end() {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(s, kj::Own<SpanImpl>) {
      s->end();
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanImpl>) {
      s->end();
    }
  }
}

}  // namespace workerd::api::user_tracing

// ======================================================================================
// Tracing

namespace workerd::api {

v8::Local<v8::Value> Tracing::enterSpan(jsg::Lock& js,
    kj::String operationName,
    v8::Local<v8::Function> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::Value>>& valuePromiseHandler) {
  // We use qualified `user_tracing::Span` / `user_tracing::SpanImpl` throughout because an
  // unqualified `Span` in this namespace resolves to workerd::Span (the runtime span struct),
  // which is a different type.

  // Cap operation name length at the API boundary so every downstream submitter sees the
  // truncated value.
  if (operationName.size() > user_tracing::MAX_USER_OPERATION_NAME_BYTES) {
    operationName = kj::str(operationName.first(user_tracing::MAX_USER_OPERATION_NAME_BYTES));
  }

  kj::Own<user_tracing::SpanImpl> impl;
  kj::Maybe<SpanParent> childSpanForAsyncContext;

  if (IoContext::hasCurrent()) {
    auto& context = IoContext::current();
    SpanParent parent = context.getCurrentUserTraceSpan();

    if (parent.isObserved()) {
      KJ_IF_SOME(observer, parent.getObserver()) {
        // newChildFromUserCode (vs newChild) signals user-origin to the submitter so it can
        // skip the operation-name allowlist that gates runtime spans.
        auto childObserver = observer.newChildFromUserCode();
        impl = kj::refcounted<user_tracing::SpanImpl>(
            kj::mv(childObserver), kj::ConstString(kj::heapString(operationName)));
        // Capture a SpanParent for the child so we can push it onto the AsyncContextFrame
        // below. Safe to carry across the request boundary thanks to BaseTracer::WeakRef in
        // the submitter - stale parents cannot pin the tracer.
        childSpanForAsyncContext = impl->makeSpanParent();
      } else {
        impl = kj::refcounted<user_tracing::SpanImpl>(nullptr);
      }
    } else {
      impl = kj::refcounted<user_tracing::SpanImpl>(nullptr);
    }
  } else {
    // No IoContext: callback still runs, but with a no-op span and no async-context push.
    impl = kj::refcounted<user_tracing::SpanImpl>(nullptr);
  }

  // Wrap impl in IoOwn (when inside an IoContext) so destruction funnels through the
  // IoContext's delete queue and cannot cross threads. Outside an IoContext, fall back to
  // kj::Own; enterSpan without an IoContext is a no-op tracing-wise but still runs the
  // callback.
  jsg::Ref<user_tracing::Span> jsSpan = [&]() -> jsg::Ref<user_tracing::Span> {
    if (IoContext::hasCurrent()) {
      auto ownedImpl = IoContext::current().addObject(kj::mv(impl));
      return js.alloc<user_tracing::Span>(kj::mv(ownedImpl));
    }
    return js.alloc<user_tracing::Span>(kj::mv(impl));
  }();

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
      // If the callback returned a promise, defer end() until settlement.
      if (result->IsPromise()) {
        auto promise = KJ_ASSERT_NONNULL(valuePromiseHandler.tryUnwrap(js, result))
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
        // destroyed (via ~SpanImpl calling end()), though this is a corner case and should
        // generally be avoided by users.
        return valuePromiseHandler.wrap(js, kj::mv(promise));
      } else {
        // Synchronous success: end immediately.
        jsSpan->end();
        return result;
      }
    }, [&](jsg::Value exception) -> v8::Local<v8::Value> {
      // Synchronous exception: end then rethrow.
      jsSpan->end();
      js.throwException(kj::mv(exception));
    });
  };

  // If we have an IoContext and an observed child span, push it onto the AsyncContextFrame
  // for the duration of the callback. The StorageScope RAII object restores the prior
  // async-context storage on scope exit; any async continuations captured during the
  // callback will already have snapshotted the new frame and will see our child span as
  // "current".
  KJ_IF_SOME(span, kj::mv(childSpanForAsyncContext)) {
    auto& context = IoContext::current();
    jsg::AsyncContextFrame::StorageScope traceScope =
        context.makeUserAsyncTraceScope(context.getCurrentLock(), kj::mv(span));
    return executeCallback();
  } else {
    return executeCallback();
  }
}

}  // namespace workerd::api
