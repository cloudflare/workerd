// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "v8-platform-wrapper.h"
#include <v8-isolate.h>
#include "jsg.h"

namespace workerd::jsg {

V8PlatformWrapper::JobTaskWrapper::JobTaskWrapper(std::unique_ptr<v8::JobTask> inner)
    : inner(kj::mv(inner)) {}

void V8PlatformWrapper::JobTaskWrapper::Run(v8::JobDelegate* delegate) {
  runInV8Stack([&](jsg::V8StackScope& stackScope) {
    inner->Run(delegate);
  });
}

}  // namespace workerd::jsg
