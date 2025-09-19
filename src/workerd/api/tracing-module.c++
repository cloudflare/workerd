// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracing-module.h"

#include "src/workerd/jsg/inspector.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>

namespace workerd::api {

jsg::Ref<JsSpanBuilder> TracingModule::startSpan(jsg::Lock& js, const kj::String name) {
  auto& ioContext = IoContext::current();
  auto spanName = kj::ConstString(kj::str(name));
  auto span = ioContext.makeUserTraceSpan(kj::mv(spanName));
  return js.alloc<JsSpanBuilder>(kj::mv(span));
}

void JsSpanBuilder::setTag(jsg::Lock& js, const kj::String key, const jsg::Value& value) {
  auto handle = value.getHandle(js);
  if (handle->IsBoolean()) {
    span.setTag(kj::ConstString(kj::str(key)), js.toBool(handle));
  } else if (handle->IsNumber()) {
    // span->setTag(kj::ConstString(kj::str(key)), lock.toDouble(handle));
  } else if (handle->IsString()) {
    span.setTag(kj::ConstString(kj::str(key)), js.toString(handle));
  } else {
    js.throwException(js.error("Unsupported tag value type"));
  }
}

}  // namespace workerd::api
