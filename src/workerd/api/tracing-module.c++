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

}  // namespace workerd::api
