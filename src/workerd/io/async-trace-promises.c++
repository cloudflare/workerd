// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-trace-promises.h"

#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>

#include <v8-context.h>
#include <v8-isolate.h>
#include <v8-primitive.h>

#include <kj/debug.h>

namespace workerd {

kj::Own<AsyncTracePromiseHook> AsyncTracePromiseHook::install(v8::Isolate* isolate) {
  auto result = kj::heap<AsyncTracePromiseHook>(isolate);
  isolate->SetPromiseHook(&hook);
  return result;
}

AsyncTracePromiseHook::AsyncTracePromiseHook(v8::Isolate* isolate) {
  v8::HandleScope scope(isolate);
  idKey.Reset(isolate, v8::Private::New(isolate));
}

AsyncTracePromiseHook::~AsyncTracePromiseHook() noexcept(false) {}

void AsyncTracePromiseHook::hook(
    v8::PromiseHookType type, v8::Local<v8::Promise> promise, v8::Local<v8::Value> parent) {
  // Promises outside traced turns are not tracked; this is the common, cheap exit.
  KJ_IF_SOME(tracker, AsyncTracker::inTurn()) {
    auto* isolate = v8::Isolate::GetCurrent();
    auto* workerIsolate = static_cast<Worker::Isolate*>(isolate->GetData(jsg::SET_DATA_ISOLATE));
    if (workerIsolate == nullptr) return;
    KJ_IF_SOME(self, workerIsolate->tryGetAsyncTracePromiseHook()) {
      // We're inside V8; nothing may propagate out.
      KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
        self.handle(tracker, isolate, type, promise, parent);
      })) {
        KJ_LOG(ERROR, "async trace promise hook failed", exception);
      }
    }
  }
}

void AsyncTracePromiseHook::handle(const AsyncTracker& tracker,
    v8::Isolate* isolate,
    v8::PromiseHookType type,
    v8::Local<v8::Promise> promise,
    v8::Local<v8::Value> parent) const {
  v8::HandleScope scope(isolate);
  auto context = isolate->GetCurrentContext();
  if (context.IsEmpty()) return;
  auto key = idKey.Get(isolate);

  // The ID stored on a promise, or 0. It may belong to another context's tracker.
  auto storedId = [&](v8::Local<v8::Promise> p) -> AsyncId {
    v8::Local<v8::Value> value;
    if (!p->GetPrivate(context, key).ToLocal(&value) || !value->IsNumber()) return 0;
    return static_cast<AsyncId>(value.As<v8::Number>()->Value());
  };
  // The promise's ID, if this tracker knows it.
  auto idOf = [&](v8::Local<v8::Promise> p) -> AsyncId {
    AsyncId id = storedId(p);
    return id != 0 && tracker.knows(id) ? id : 0;
  };

  switch (type) {
    case v8::PromiseHookType::kInit: {
      AsyncId parentId = parent->IsPromise() ? idOf(parent.As<v8::Promise>()) : 0;
      AsyncId id = tracker.createPromise(parentId);
      // IDs are below 2^53, so a Number holds them exactly.
      if (id != 0) {
        (void)promise->SetPrivate(context, key, v8::Number::New(isolate, static_cast<double>(id)));
      }
      return;
    }
    case v8::PromiseHookType::kBefore: {
      if (AsyncId id = idOf(promise); id != 0) tracker.enterPromise(id);
      return;
    }
    case v8::PromiseHookType::kAfter: {
      // By now the tracker has usually forgotten the promise: a reaction's promise settles during
      // the reaction. Its callback scope is still the innermost one, if kBefore entered it.
      if (AsyncId id = storedId(promise); id != 0 && tracker.current() == id) {
        tracker.exitPromise(id);
      }
      return;
    }
    case v8::PromiseHookType::kResolve: {
      if (AsyncId id = idOf(promise); id != 0) {
        tracker.settlePromise(id,
            promise->State() == v8::Promise::kRejected ? AsyncOutcome::ERROR : AsyncOutcome::OK);
      }
      return;
    }
  }
}

}  // namespace workerd
