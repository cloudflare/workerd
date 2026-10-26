// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Creation stack capture for async tracing, with V8.

#include <workerd/io/async-trace.h>

namespace v8 {
class Isolate;
}

namespace workerd {

// Captures up to `maxFrames` frames of `isolate`'s current JavaScript stack (function, script
// name, script ID, line, column), only while `isolate` is current and locked on the calling thread.
// `isolate` must outlive the capturer's use (trackers stop creating resources when their context
// ends).
kj::Arc<const AsyncStackCapturer> newAsyncStackCapturer(v8::Isolate* isolate, uint32_t maxFrames);

}  // namespace workerd
