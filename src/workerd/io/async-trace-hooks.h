// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "async-trace.h"
#include "io-context.h"

#include <v8.h>

namespace workerd {

// V8 Promise Hook for async tracing.
// This hooks into V8's promise lifecycle to track JS promise creation and execution.
//
// The hook is installed per-isolate but uses IoContext::tryCurrent() to get the
// per-request AsyncTraceContext.
class AsyncTracePromiseHook {
 public:
  // Install the promise hook on an isolate.
  // Should be called during isolate setup.
  static void install(v8::Isolate* isolate);

 private:
  // The actual hook callback
  static void promiseHook(
      v8::PromiseHookType type, v8::Local<v8::Promise> promise, v8::Local<v8::Value> parent);

  // Handle promise init (creation)
  static void onInit(v8::Isolate* isolate,
      AsyncTraceContext& trace,
      v8::Local<v8::Promise> promise,
      v8::Local<v8::Value> parent);

  // Handle before callback (about to run .then() handler)
  static void onBefore(
      v8::Isolate* isolate, AsyncTraceContext& trace, v8::Local<v8::Promise> promise);

  // Handle after callback (finished running .then() handler)
  static void onAfter(
      v8::Isolate* isolate, AsyncTraceContext& trace, v8::Local<v8::Promise> promise);

  // Handle resolve (promise settled)
  static void onResolve(
      v8::Isolate* isolate, AsyncTraceContext& trace, v8::Local<v8::Promise> promise);
};

// Helper class to get AsyncTraceContext from current IoContext.
// Returns nullptr if tracing is not enabled for this request.
inline AsyncTraceContext* tryGetAsyncTrace() {
  KJ_IF_SOME(ctx, IoContext::tryCurrent()) {
    return ctx.getAsyncTrace();
  }
  return nullptr;
}

}  // namespace workerd
