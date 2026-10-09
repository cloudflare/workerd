// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Tier 2 async tracing: JavaScript promises, through V8's promise hook. Local development only.
//
// Installing a promise hook permanently sends the isolate's promise operations through V8's slow
// path, traced or not, so it is installed only on isolates configured for it
// (AsyncTraceConfig::promises), when they are created, and never removed.
//
// While a traced turn runs (AsyncTracker::inTurn()), each promise becomes a JS_PROMISE resource:
//   - init: created, triggered by the promise it derives from if that is tracked by the same
//     tracker, else by the turn's cause. Its ID is kept on the promise under a private symbol.
//   - before/after: its reaction runs as a callback.
//   - resolve: settled (OK, ERROR if rejected; a promise resolved with a thenable counts as OK).
// Promises created outside traced turns, or tracked by another context's tracker, are ignored.

#include <workerd/io/async-trace.h>

#include <v8-local-handle.h>
#include <v8-persistent-handle.h>
#include <v8-promise.h>

namespace v8 {
class Isolate;
class Private;
}  // namespace v8

namespace workerd {

class AsyncTracePromiseHook {
 public:
  // Installs the hook on `isolate`, which must be locked and belong to a Worker::Isolate. Call
  // once, at isolate creation. The Worker::Isolate must own the result, returning it from
  // tryGetAsyncTracePromiseHook().
  static kj::Own<AsyncTracePromiseHook> install(v8::Isolate* isolate);

  explicit AsyncTracePromiseHook(v8::Isolate* isolate);
  ~AsyncTracePromiseHook() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(AsyncTracePromiseHook);

 private:
  // Holds a promise's resource ID (a Number).
  v8::Global<v8::Private> idKey;

  static void hook(
      v8::PromiseHookType type, v8::Local<v8::Promise> promise, v8::Local<v8::Value> parent);
  void handle(const AsyncTracker& tracker,
      v8::Isolate* isolate,
      v8::PromiseHookType type,
      v8::Local<v8::Promise> promise,
      v8::Local<v8::Value> parent) const;
};

}  // namespace workerd
