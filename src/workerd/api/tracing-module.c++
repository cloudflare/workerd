// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracing-module.h"

namespace workerd::api {

JsSpan::JsSpan(kj::Maybe<IoOwn<SpanBuilder>> span): span(kj::mv(span)) {}

JsSpan::~JsSpan() noexcept(false) {
  end();
}

void JsSpan::end() {
  KJ_IF_SOME(s, span) {
    s->end();
    span = kj::none;
  }
}

void JsSpan::setTag(
    jsg::Lock& js, kj::String key, jsg::Optional<kj::OneOf<bool, double, kj::String>> maybeValue) {
  KJ_IF_SOME(s, span) {
    KJ_IF_SOME(value, maybeValue) {
      // JavaScript numbers (double) are stored as-is, not converted to int64_t
      s->setTag(kj::ConstString(kj::mv(key)), kj::mv(value));
    }
    // If value is undefined/none, we simply don't set the tag
  }
}

jsg::Ref<JsSpan> TracingModule::startSpan(jsg::Lock& js, const kj::String name) {
  auto spanName = kj::ConstString(kj::str(name));

  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    auto spanBuilder = ioContext.makeUserTraceSpan(kj::mv(spanName));
    auto ownedSpan = ioContext.addObject(kj::heap(kj::mv(spanBuilder)));
    return js.alloc<JsSpan>(kj::mv(ownedSpan));
  } else {
    // When no IoContext is available, create a no-op span
    return js.alloc<JsSpan>(kj::none);
  }
}

}  // namespace workerd::api
