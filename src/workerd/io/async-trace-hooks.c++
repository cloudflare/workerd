// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-trace-hooks.h"

namespace workerd {

void AsyncTracePromiseHook::install(v8::Isolate* isolate) {
  isolate->SetPromiseHook(&promiseHook);
}

void AsyncTracePromiseHook::promiseHook(
    v8::PromiseHookType type, v8::Local<v8::Promise> promise, v8::Local<v8::Value> parent) {

  // Get the AsyncTraceContext for the current request, if any
  auto* trace = tryGetAsyncTrace();
  if (trace == nullptr) {
    return;  // Tracing not enabled for this request
  }

  auto* isolate = v8::Isolate::GetCurrent();

  switch (type) {
    case v8::PromiseHookType::kInit:
      onInit(isolate, *trace, promise, parent);
      break;
    case v8::PromiseHookType::kBefore:
      onBefore(isolate, *trace, promise);
      break;
    case v8::PromiseHookType::kAfter:
      onAfter(isolate, *trace, promise);
      break;
    case v8::PromiseHookType::kResolve:
      onResolve(isolate, *trace, promise);
      break;
  }
}

void AsyncTracePromiseHook::onInit(v8::Isolate* isolate,
    AsyncTraceContext& trace,
    v8::Local<v8::Promise> promise,
    v8::Local<v8::Value> parent) {

  // Determine the trigger ID based on the current execution context.
  // The current() ID represents which resource's callback we're inside.
  AsyncTraceContext::AsyncId triggerId = trace.current();

  // Only use the V8 parent promise as trigger if we're in the root context.
  // When we're inside a callback (bridge, promise, etc.), we want to preserve
  // that context as the trigger so the visualization shows what triggered
  // the new promise creation from the caller's perspective.
  if (triggerId == AsyncTraceContext::ROOT_ID) {
    // We're not inside any callback, so use parent promise if available
    if (!parent.IsEmpty() && parent->IsPromise()) {
      auto parentPromise = parent.As<v8::Promise>();
      if (trace.hasPromiseAsyncId(isolate, parentPromise)) {
        triggerId = trace.getPromiseAsyncId(isolate, parentPromise);
      }
    }
  }

  // Create a new async resource for this promise
  auto asyncId = trace.createResourceWithTrigger(
      AsyncTraceContext::ResourceType::kJsPromise, triggerId, isolate);

  // Store the async ID on the promise using a private symbol
  trace.setPromiseAsyncId(isolate, promise, asyncId);
}

void AsyncTracePromiseHook::onBefore(
    v8::Isolate* isolate, AsyncTraceContext& trace, v8::Local<v8::Promise> promise) {

  // Get the async ID for this promise
  if (!trace.hasPromiseAsyncId(isolate, promise)) {
    return;  // Promise wasn't tracked (created before tracing started?)
  }

  auto asyncId = trace.getPromiseAsyncId(isolate, promise);
  if (asyncId == AsyncTraceContext::INVALID_ID) {
    return;
  }

  // Enter the callback context for this promise
  trace.enterCallback(asyncId);
}

void AsyncTracePromiseHook::onAfter(
    v8::Isolate* isolate, AsyncTraceContext& trace, v8::Local<v8::Promise> promise) {

  // Get the async ID for this promise
  if (!trace.hasPromiseAsyncId(isolate, promise)) {
    return;
  }

  auto asyncId = trace.getPromiseAsyncId(isolate, promise);
  if (asyncId == AsyncTraceContext::INVALID_ID) {
    return;
  }

  // Exit the callback context
  // Note: We need to be careful here - V8's promise hooks don't always
  // give us matching before/after pairs in the order we expect.
  // The AsyncTraceContext maintains a stack, so this should work correctly
  // as long as callbacks don't overlap incorrectly.
  trace.exitCallback();
}

void AsyncTracePromiseHook::onResolve(
    v8::Isolate* isolate, AsyncTraceContext& trace, v8::Local<v8::Promise> promise) {

  // When a promise resolves, we could mark it as "resolved" in our trace.
  // For now, we don't do anything special here - the important timing
  // is captured in onBefore/onAfter.
  //
  // In the future, we might want to track:
  // - Time between promise creation and resolution
  // - Whether the promise was fulfilled or rejected
}

}  // namespace workerd
