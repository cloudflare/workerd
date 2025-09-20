// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracing-module.h"

#include "src/workerd/jsg/inspector.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>

namespace workerd::api {

// InternalSpan implementation
InternalSpan::InternalSpan(kj::Maybe<IoOwn<InternalSpanImpl>> impl): impl(kj::mv(impl)) {}

void InternalSpan::end() {
  KJ_IF_SOME(s, impl) {
    s->end();
  }
}

void InternalSpan::setTag(
    jsg::Lock& js, kj::String key, kj::Maybe<kj::OneOf<bool, double, kj::String>> maybeValue) {
  KJ_IF_SOME(value, maybeValue) {
    kj::OneOf<bool, double, kj::String> tagValue;

    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(s, kj::String) {
        tagValue = kj::mv(s);
      }
      KJ_CASE_ONEOF(b, bool) {
        tagValue = b;
      }
      KJ_CASE_ONEOF(d, double) {
        tagValue = d;
      }
    }

    KJ_IF_SOME(s, impl) {
      s->setTag(kj::ConstString(kj::str(kj::mv(key))), kj::mv(tagValue));
    }
  }
}

bool InternalSpan::getIsRecording() {
  KJ_IF_SOME(s, impl) {
    return s->getIsRecording();
  }
  return false;
}

// InternalSpanImpl implementation
InternalSpanImpl::InternalSpanImpl(SpanBuilder span): span(kj::mv(span)) {}

InternalSpanImpl::~InternalSpanImpl() noexcept(false) {
  end();
}

void InternalSpanImpl::end() {
  KJ_IF_SOME(s, span) {
    s.end();
    span = kj::none;
  }
}

void InternalSpanImpl::setTag(kj::ConstString key, kj::OneOf<bool, double, kj::String> value) {
  KJ_IF_SOME(s, span) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(b, bool) {
        s.setTag(kj::mv(key), b);
      }
      KJ_CASE_ONEOF(d, double) {
        s.setTag(kj::mv(key), d);
      }
      KJ_CASE_ONEOF(str, kj::String) {
        s.setTag(kj::mv(key), kj::mv(str));
      }
    }
  }
}

bool InternalSpanImpl::getIsRecording() {
  return span != kj::none;
}

SpanParent InternalSpanImpl::makeSpanParent() {
  KJ_IF_SOME(s, span) {
    return SpanParent(s);
  } else {
    return SpanParent(nullptr);
  }
}

SpanParent InternalSpan::makeSpanParent() {
  KJ_IF_SOME(s, impl) {
    return s->makeSpanParent();
  }
  return SpanParent(nullptr);
}

jsg::Ref<InternalSpan> TracingModule::startSpan(jsg::Lock& js, const kj::String name) {
  auto spanName = kj::ConstString(kj::str(name));

  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    auto span = ioContext.makeUserTraceSpan(kj::mv(spanName));
    auto impl = kj::heap<InternalSpanImpl>(kj::mv(span));
    return js.alloc<InternalSpan>(ioContext.addObject(kj::mv(impl)));
  } else {
    // When no IoContext is available, create a no-op span
    return js.alloc<InternalSpan>(kj::none);
  }
}

jsg::JsValue TracingModule::startSpanWithCallback(jsg::Lock& js,
    kj::String operationName,
    jsg::Function<jsg::Value(jsg::Arguments<jsg::Value>)> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<InternalSpan>>& jsSpanHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::Value>>& valuePromiseHandler) {

  // Get the current span parent
  if (!IoContext::hasCurrent()) {
    // Create a resolver and promise
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(js.v8Context()).ToLocal(&resolver)) {
      return js.undefined();
    }
    auto promise = resolver->GetPromise();

    // Prepare callback arguments - create a new Arguments with span prepended
    kj::Vector<jsg::Value> callbackArgVector;
    // Add a no-op span as first argument
    auto noOpSpan = js.alloc<InternalSpan>(kj::none);
    callbackArgVector.add(js.v8Ref(jsSpanHandler.wrap(js, kj::mv(noOpSpan))));
    for (auto& arg: args) {
      callbackArgVector.add(arg.addRef(js));
    }
    auto callbackArgs = jsg::Arguments<jsg::Value>(callbackArgVector.releaseAsArray());

    // Execute callback directly
    auto result = js.tryCatch([&]() -> jsg::JsValue {
      auto callbackResult = callback(js, kj::mv(callbackArgs));
      return jsg::JsValue(callbackResult.getHandle(js));
    }, [&](jsg::Value exception) -> jsg::JsValue {
      resolver->Reject(js.v8Context(), exception.getHandle(js));
      return jsg::JsValue(promise);
    });

    // If result is a promise, chain it to our resolver
    v8::Local<v8::Value> resultHandle = result;
    if (resultHandle->IsPromise()) {
      auto resultPromise = resultHandle.As<v8::Promise>();
      // For simplicity, just return the result promise directly
      return result;
    } else {
      // Resolve with the result
      resolver->Resolve(js.v8Context(), resultHandle);
      return jsg::JsValue(promise);
    }
  }

  SpanParent parent = IoContext::current().getCurrentTraceSpan();

  // Create the span implementation
  auto spanName = kj::ConstString(kj::str(operationName));

  auto& ioContext = IoContext::current();
  auto span = ioContext.makeUserTraceSpan(kj::mv(spanName));
  auto impl = kj::heap<InternalSpanImpl>(kj::mv(span));
  // Create the JavaScript span object
  jsg::Ref<InternalSpan> jsSpan = js.alloc<InternalSpan>(ioContext.addObject(kj::mv(impl)));

  // Create new span parent for the callback execution context
  SpanParent newSpanParent = jsSpan->makeSpanParent();

  // Prepare callback arguments - create a new Arguments with span prepended
  kj::Vector<jsg::Value> callbackArgVector;
  callbackArgVector.add(js.v8Ref(jsSpanHandler.wrap(js, jsSpan.addRef())));
  for (auto& arg: args) {
    callbackArgVector.add(arg.addRef(js));
  }
  auto callbackArgs = jsg::Arguments<jsg::Value>(callbackArgVector.releaseAsArray());

  // Define callback execution logic
  auto executeCallback = [&jsSpan, &js, &callback, callbackArgs = kj::mv(callbackArgs),
                             &valuePromiseHandler]() mutable -> jsg::JsValue {
    auto v8Context = js.v8Context();
    return js.tryCatch([&]() -> jsg::JsValue {
      auto callbackResult = callback(js, kj::mv(callbackArgs));
      v8::Local<v8::Value> result = callbackResult.getHandle(js);

      // Check if result is a promise for async handling
      if (result->IsPromise()) {
        auto promise = KJ_ASSERT_NONNULL(valuePromiseHandler.tryUnwrap(js, result))
                           .then(js,
                               [jsSpan = jsSpan.addRef()](
                                   jsg::Lock& js, jsg::Value value) mutable -> jsg::Value {
          // Call end() at the end of async scope on success
          jsSpan->end();
          return kj::mv(value);
        },
                               [jsSpan = jsSpan.addRef()](
                                   jsg::Lock& js, jsg::Value exception) mutable -> jsg::Value {
          // Call end() at the end of async scope on exception
          jsSpan->end();
          js.throwException(kj::mv(exception));
        });
        return jsg::JsValue(valuePromiseHandler.wrap(js, kj::mv(promise)));
      } else {
        // Call end() at the end of synchronous scope on success
        jsSpan->end();
        return jsg::JsValue(result);
      }
    }, [&](jsg::Value exception) -> jsg::JsValue {
      // Call end() at the end of synchronous scope on exception
      jsSpan->end();
      js.throwException(kj::mv(exception));
    });
  };
  return executeCallback();
}

}  // namespace workerd::api
