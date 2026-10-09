// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Reports async trace events (async-trace.h) to V8's inspector as async tasks, so that DevTools
// async stack traces continue across timers, I/O and binding calls, not only promises. Local
// development only: an inspector exists only outside multi-tenant processes (see worker.c++).
//
// - Creating a resource schedules a task (asyncTaskScheduled), capturing the current JavaScript
//   stack as the async parent of its callbacks. JS_TO_KJ resources are skipped: they never run a
//   callback.
// - Each callback runs as the task (asyncTaskStarted/asyncTaskFinished).
// - Destroying an unsettled resource cancels its task.
//
// V8 ignores all of this unless a session has set an async call stack depth
// (Debugger.setAsyncCallStackDepth), and bounds the number of stored stacks.

#include <workerd/io/async-trace.h>

namespace v8 {
class Isolate;
}

namespace v8_inspector {
class V8Inspector;
}

namespace workerd {

// `inspector` belongs to `isolate`; both must outlive the sink. V8 is called only while `isolate`
// is current and locked.
kj::Own<AsyncTraceListener> newAsyncTraceInspectorSink(
    v8_inspector::V8Inspector& inspector, v8::Isolate* isolate);

}  // namespace workerd
