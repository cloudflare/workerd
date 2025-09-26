// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracing-module.h"

#include "src/workerd/jsg/inspector.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>

namespace workerd::api {

InternalSpan::InternalSpan(kj::Maybe<IoOwn<SpanBuilder>> span): span(kj::mv(span)) {}

InternalSpan::~InternalSpan() noexcept(false) {
  end();
}

void InternalSpan::end() {
  KJ_IF_SOME(s, span) {
    s->end();
    span = kj::none;
  }
}

void InternalSpan::setTag(
    jsg::Lock& js, kj::String key, jsg::Optional<kj::OneOf<bool, double, kj::String>> maybeValue) {
  KJ_IF_SOME(s, span) {
    KJ_IF_SOME(value, maybeValue) {
      s->setTag(kj::ConstString(kj::mv(key)), kj::mv(value));
    }
  }
}

bool InternalSpan::getIsRecording() {
  return span != kj::none;
}

SpanParent InternalSpan::makeSpanParent() {
  KJ_IF_SOME(s, span) {
    return SpanParent(*s);
  }
  return SpanParent(nullptr);
}

jsg::Ref<InternalSpan> TracingModule::startSpan(jsg::Lock& js, const kj::String name) {
  auto spanName = kj::ConstString(kj::str(name));

  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    auto spanBuilder = ioContext.makeUserTraceSpan(kj::mv(spanName));
    auto ownedSpan = ioContext.addObject(kj::heap(kj::mv(spanBuilder)));
    return js.alloc<InternalSpan>(kj::mv(ownedSpan));
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

  // Create span - either real or no-op depending on IoContext availability
  jsg::Ref<InternalSpan> jsSpan = [&]() {
    if (!IoContext::hasCurrent()) {
      return js.alloc<InternalSpan>(kj::none);
    }
    auto& ioContext = IoContext::current();
    auto spanBuilder = ioContext.makeUserTraceSpan(kj::ConstString(kj::str(operationName)));
    auto ownedSpan = ioContext.addObject(kj::heap(kj::mv(spanBuilder)));
    return js.alloc<InternalSpan>(kj::mv(ownedSpan));
  }();

  // Prepare callback arguments with span prepended
  kj::Vector<jsg::Value> callbackArgVector;
  callbackArgVector.add(js.v8Ref(jsSpanHandler.wrap(js, jsSpan.addRef())));
  for (auto& arg: args) {
    callbackArgVector.add(arg.addRef(js));
  }
  auto callbackArgs = jsg::Arguments<jsg::Value>(callbackArgVector.releaseAsArray());

  // Execute callback with automatic span cleanup
  return js.tryCatch([&]() -> jsg::JsValue {
    auto callbackResult = callback(js, kj::mv(callbackArgs));
    v8::Local<v8::Value> result = callbackResult.getHandle(js);

    // Handle async callbacks - attach span cleanup to promise chain
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
      return jsg::JsValue(valuePromiseHandler.wrap(js, kj::mv(promise)));
    }

    // Handle sync callbacks - end span immediately
    jsSpan->end();
    return jsg::JsValue(result);
  }, [&](jsg::Value exception) -> jsg::JsValue {
    jsSpan->end();
    js.throwException(kj::mv(exception));
  });
}

}  // namespace workerd::api
