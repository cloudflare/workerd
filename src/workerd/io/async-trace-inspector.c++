// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-trace-inspector.h"

#include <v8-inspector.h>
#include <v8-locker.h>

#include <kj/vector.h>

namespace workerd {
namespace {

class InspectorSink final: public AsyncTraceListener {
 public:
  InspectorSink(v8_inspector::V8Inspector& inspector, v8::Isolate* isolate)
      : inspector(inspector),
        isolate(isolate) {}

  ~InspectorSink() noexcept(false) {
    finishAll();
  }

  void onInit(uint64_t ctx, const AsyncInitEvent& event) override {
    if (event.kind == AsyncKind::JS_TO_KJ || !isCurrent()) return;
    // An interval runs its callback repeatedly; every other resource runs it at most once (or,
    // for a request, without a JavaScript stack worth keeping), and V8 forgets a non-recurring
    // task once it finishes.
    bool recurring = event.kind == AsyncKind::TIMER && event.name == "setInterval"_kj.asArray();
    inspector.asyncTaskScheduled(
        v8_inspector::StringView(event.name.asBytes().begin(), event.name.size()), task(event.id),
        recurring);
  }

  void onBefore(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    if (isCurrent()) {
      inspector.asyncTaskStarted(task(id));
      started.add(id);
    } else {
      started.add(0);
    }
  }

  void onAfter(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    // The tracker can discard scopes nested inside `id` without reporting them (it counts them as
    // unbalanced); finish those too, so V8's stack of current tasks stays balanced. Finishing is
    // done even if the isolate is not current: an unmatched start would corrupt V8's stack.
    for (size_t i = started.size(); i > 0; --i) {
      if (started[i - 1] == id) {
        while (started.size() >= i) finishLast();
        return;
      }
    }
  }

  void onDestroy(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    if (isCurrent()) inspector.asyncTaskCanceled(task(id));
  }

  void onContextEnd(uint64_t ctx, uint64_t atNs, const AsyncContextStats& stats) override {
    finishAll();
  }

 private:
  v8_inspector::V8Inspector& inspector;
  v8::Isolate* isolate;
  // Callbacks begun and not yet finished, innermost last. 0 = begun while the isolate was not
  // current, so not reported to V8.
  kj::Vector<AsyncId> started;

  // V8 keys its own (promise) tasks with odd values, so ours are even.
  static void* task(AsyncId id) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(id) << 1);
  }

  bool isCurrent() const {
    return v8::Isolate::TryGetCurrent() == isolate && v8::Locker::IsLocked(isolate);
  }

  void finishLast() {
    AsyncId id = started.back();
    started.removeLast();
    if (id != 0) inspector.asyncTaskFinished(task(id));
  }

  void finishAll() {
    while (!started.empty()) finishLast();
  }
};

}  // namespace

kj::Own<AsyncTraceListener> newAsyncTraceInspectorSink(
    v8_inspector::V8Inspector& inspector, v8::Isolate* isolate) {
  return kj::heap<InspectorSink>(inspector, isolate);
}

}  // namespace workerd
