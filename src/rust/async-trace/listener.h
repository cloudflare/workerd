// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of async trace sinks. Most code should include <workerd/io/async-trace.h>, which
// re-exports everything here.
//
// This header lives in the Rust crate's package, and depends only on KJ and cxx, so that the
// bridge (ffi.rs) can call C++ listeners without a dependency cycle.

#include <rust/cxx.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/exception.h>
#include <kj/string.h>
#include <kj/vector.h>

#include <cstdint>

namespace workerd {

// Identifies an async resource within an isolate. 0 means "none".
using AsyncId = uint64_t;

// Must match `Kind::from_u8` in lib.rs.
enum class AsyncKind : uint8_t {
  REQUEST = 0,
  KJ_TO_JS = 1,
  JS_TO_KJ = 2,
  TIMER = 3,
  MICROTASK = 4,
  OPERATION = 5,
  JS_PROMISE = 6,
  OTHER = 7,
};

// Must match `Outcome::from_u8` in lib.rs.
enum class AsyncOutcome : uint8_t {
  OK = 0,
  ERROR = 1,
  CANCELED = 2,
};

struct AsyncInitEvent {
  AsyncId id;
  // The resource whose completion caused this one to be created.
  AsyncId trigger;
  // The resource whose callback was running when this one was created.
  AsyncId execution;
  AsyncKind kind;
  // Not NUL-terminated.
  kj::ArrayPtr<const char> name;
  uint64_t atNs;
  // 0 = no creation stack.
  uint32_t stack;
};

struct AsyncTurn {
  // 0 if unknown.
  AsyncId cause;
  uint64_t startNs;
  kj::Maybe<uint64_t> lockedNs;
  uint64_t endNs;
};

// One frame of a creation stack. Strings are not NUL-terminated.
struct AsyncStackFrame {
  kj::ArrayPtr<const char> function;
  kj::ArrayPtr<const char> script;
  int32_t scriptId;
  // 1-based, as V8 reports them.
  uint32_t line;
  uint32_t column;
};

// See `ContextStats` in lib.rs. All zero means the trace is complete.
struct AsyncContextStats {
  uint64_t created;
  uint64_t dropped;
  uint64_t unknown;
  uint64_t unbalanced;
  uint64_t ambiguousBindings;
  uint64_t foreignThread;
};

// Receives the events of one tracker (one IoContext). Times are nanoseconds since the process
// trace epoch.
//
// A listener is called only on the tracker's thread, and is destroyed there when the tracker
// closes. Exceptions thrown by a listener are logged and otherwise ignored.
class AsyncTraceListener {
 public:
  virtual ~AsyncTraceListener() noexcept(false) = default;

  virtual void onContextBegin(uint64_t ctx, uint64_t isolate) {}
  virtual void onInit(uint64_t ctx, const AsyncInitEvent& event) = 0;
  virtual void onSettle(uint64_t ctx, AsyncId id, AsyncOutcome outcome, uint64_t atNs) {}
  virtual void onBefore(uint64_t ctx, AsyncId id, uint64_t atNs) {}
  virtual void onAfter(uint64_t ctx, AsyncId id, uint64_t atNs) {}
  virtual void onDestroy(uint64_t ctx, AsyncId id, uint64_t atNs) {}
  // `key` and `value` are not NUL-terminated.
  virtual void onAnnotate(
      uint64_t ctx, AsyncId id, kj::ArrayPtr<const char> key, kj::ArrayPtr<const char> value) {}
  virtual void onTurn(uint64_t ctx, const AsyncTurn& turn) {}
  // Creation stack `id` of `isolate`, innermost frame first. Reported to a listener before the
  // first onInit() that refers to it, once per listener.
  virtual void onStack(uint64_t isolate, uint32_t id, kj::ArrayPtr<const AsyncStackFrame> frames) {}
  virtual void onContextEnd(uint64_t ctx, uint64_t atNs, const AsyncContextStats& stats) {}
};

namespace rust::async_trace {

// Shims for the Rust `CppSink`. Each one contains the listener's exceptions, which would otherwise
// become a Rust panic at the bridge.

template <typename Func>
inline void callListener(Func&& func) {
  KJ_IF_SOME(exception, kj::runCatchingExceptions(kj::fwd<Func>(func))) {
    KJ_LOG(ERROR, "async trace listener threw", exception);
  }
}

inline kj::ArrayPtr<const char> fromRust(::rust::Str str) {
  return kj::arrayPtr(str.data(), str.size());
}

inline void listener_context_begin(AsyncTraceListener& listener, uint64_t ctx, uint64_t isolate) {
  callListener([&]() { listener.onContextBegin(ctx, isolate); });
}

inline void listener_init(AsyncTraceListener& listener,
    uint64_t ctx,
    uint64_t id,
    uint64_t trigger,
    uint64_t execution,
    uint8_t kind,
    ::rust::Str name,
    uint64_t at,
    uint32_t stack) {
  AsyncInitEvent event{
    .id = id,
    .trigger = trigger,
    .execution = execution,
    .kind = static_cast<AsyncKind>(kind),
    .name = fromRust(name),
    .atNs = at,
    .stack = stack,
  };
  callListener([&]() { listener.onInit(ctx, event); });
}

inline void listener_settle(
    AsyncTraceListener& listener, uint64_t ctx, uint64_t id, uint8_t outcome, uint64_t at) {
  callListener([&]() { listener.onSettle(ctx, id, static_cast<AsyncOutcome>(outcome), at); });
}

inline void listener_before(AsyncTraceListener& listener, uint64_t ctx, uint64_t id, uint64_t at) {
  callListener([&]() { listener.onBefore(ctx, id, at); });
}

inline void listener_after(AsyncTraceListener& listener, uint64_t ctx, uint64_t id, uint64_t at) {
  callListener([&]() { listener.onAfter(ctx, id, at); });
}

inline void listener_destroy(AsyncTraceListener& listener, uint64_t ctx, uint64_t id, uint64_t at) {
  callListener([&]() { listener.onDestroy(ctx, id, at); });
}

inline void listener_annotate(
    AsyncTraceListener& listener, uint64_t ctx, uint64_t id, ::rust::Str key, ::rust::Str value) {
  callListener([&]() { listener.onAnnotate(ctx, id, fromRust(key), fromRust(value)); });
}

// A stack arrives frame by frame (see `listener_stack_begin` in ffi.rs), buffered here until
// listener_stack_end(). Listeners are called on their tracker's thread, and the calls for one
// stack are never interleaved with another's.
struct PendingStackFrame {
  kj::String function;
  kj::String script;
  int32_t scriptId;
  uint32_t line;
  uint32_t column;
};

inline kj::Vector<PendingStackFrame>& pendingStack() {
  static thread_local kj::Vector<PendingStackFrame> frames;
  return frames;
}

inline void listener_stack_begin(AsyncTraceListener& listener) {
  pendingStack().clear();
}

inline void listener_stack_frame(AsyncTraceListener& listener,
    ::rust::Str function,
    ::rust::Str script,
    int32_t scriptId,
    uint32_t line,
    uint32_t column) {
  pendingStack().add(PendingStackFrame{
    .function = kj::heapString(function.data(), function.size()),
    .script = kj::heapString(script.data(), script.size()),
    .scriptId = scriptId,
    .line = line,
    .column = column,
  });
}

inline void listener_stack_end(AsyncTraceListener& listener, uint64_t isolate, uint32_t id) {
  auto& pending = pendingStack();
  auto frames = KJ_MAP(f, pending) {
    return AsyncStackFrame{
      .function = f.function.asArray(),
      .script = f.script.asArray(),
      .scriptId = f.scriptId,
      .line = f.line,
      .column = f.column,
    };
  };
  callListener([&]() { listener.onStack(isolate, id, frames); });
  pending.clear();
}

// `locked` is meaningful only if `hasLocked`.
inline void listener_turn(AsyncTraceListener& listener,
    uint64_t ctx,
    uint64_t cause,
    uint64_t start,
    bool hasLocked,
    uint64_t locked,
    uint64_t end) {
  AsyncTurn turn{
    .cause = cause,
    .startNs = start,
    .lockedNs = hasLocked ? kj::Maybe<uint64_t>(locked) : kj::none,
    .endNs = end,
  };
  callListener([&]() { listener.onTurn(ctx, turn); });
}

inline void listener_context_end(AsyncTraceListener& listener,
    uint64_t ctx,
    uint64_t at,
    uint64_t created,
    uint64_t dropped,
    uint64_t unknown,
    uint64_t unbalanced,
    uint64_t ambiguousBindings,
    uint64_t foreignThread) {
  AsyncContextStats stats{
    .created = created,
    .dropped = dropped,
    .unknown = unknown,
    .unbalanced = unbalanced,
    .ambiguousBindings = ambiguousBindings,
    .foreignThread = foreignThread,
  };
  callListener([&]() { listener.onContextEnd(ctx, at, stats); });
}

}  // namespace rust::async_trace
}  // namespace workerd
