// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Async activity tracking, in the style of Node.js `async_hooks`. The state and the NDJSON output
// are implemented in Rust (//src/rust/async-trace); this is the C++ facade over them.
//
// An IoContext has an AsyncTracker only when the isolate's IsolateObserver enables async tracing
// (getAsyncTraceConfig()) and at least one sink is registered for the context
// (addAsyncTraceSinks()). Otherwise IoContext::tryGetAsyncTracker() returns none, and every
// instrumentation site costs one null check: an AsyncResource is then inert, and the scopes do
// nothing.

#include <workerd/rust/async-trace/listener.h>

#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/vector.h>

#include <atomic>

namespace workerd {

namespace rust::async_trace {
struct Isolate;
struct Tracker;
struct Writer;
}  // namespace rust::async_trace

class AsyncTracker;

namespace _ {  // private
// The tracker of the innermost open AsyncTracker::TurnScope on this thread, or null if that turn
// is not traced (or there is none).
extern thread_local const AsyncTracker* trackerInTurn;
}  // namespace _

// Receives the frames of a creation stack, innermost first. See AsyncStackCapturer.
class AsyncStackBuilder {
 public:
  // Strings need not be valid UTF-8 or NUL-terminated. `line` and `column` are 1-based.
  void addFrame(kj::ArrayPtr<const char> function,
      kj::ArrayPtr<const char> script,
      int32_t scriptId,
      uint32_t line,
      uint32_t column);

 private:
  explicit AsyncStackBuilder(rust::async_trace::Tracker& tracker): tracker(tracker) {}
  rust::async_trace::Tracker& tracker;
  friend class AsyncTracker;
};

// Captures the current JavaScript stack when a resource is created. Implemented by the isolate's
// owner, which can reach V8 (this layer can't). Called on the isolate's thread, possibly while no
// JavaScript is running.
class AsyncStackCapturer: public kj::AtomicRefcounted {
 public:
  virtual ~AsyncStackCapturer() noexcept(false) = default;

  // Adds the current stack's frames, or none if the isolate isn't running JavaScript on this
  // thread.
  virtual void capture(AsyncStackBuilder& builder) const = 0;
};

// Per-isolate state: resource ID allocation and stack deduplication. Owned by Worker::Isolate when
// the isolate has async tracing enabled. Thread-safe.
class AsyncTraceIsolate {
 public:
  // With a `stackCapturer`, resources created in this isolate record their creation stacks.
  explicit AsyncTraceIsolate(kj::Maybe<kj::Arc<const AsyncStackCapturer>> stackCapturer = kj::none);
  ~AsyncTraceIsolate() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(AsyncTraceIsolate);

 private:
  ::rust::Box<rust::async_trace::Isolate> impl;
  kj::Maybe<kj::Arc<const AsyncStackCapturer>> stackCapturer;
  friend class AsyncTracker;
};

// A process-wide NDJSON output file (format: //src/rust/async-trace/ndjson.rs). Thread-safe.
class AsyncTraceWriter {
 public:
  // Creates or truncates `path` and writes the header. Throws if that fails.
  static kj::Own<AsyncTraceWriter> open(kj::StringPtr path, kj::StringPtr producerVersion);

  explicit AsyncTraceWriter(::rust::Box<rust::async_trace::Writer> impl);
  ~AsyncTraceWriter() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(AsyncTraceWriter);

  // Whether an I/O error has disabled the writer. Later events are dropped.
  bool failed() const;

  // Writes buffered events to the file. Sinks flush at the end of each outermost turn; call this
  // before exiting without running destructors.
  void flush() const;

 private:
  ::rust::Box<rust::async_trace::Writer> impl;
  friend class AsyncTracker;
};

// Collects the sinks for one IoContext, before its tracker exists.
class AsyncTraceSinks {
 public:
  void add(kj::Own<AsyncTraceListener> listener);

  // Events are serialized in Rust, with no C++ call per event. The tracker shares ownership of the
  // underlying file, so `writer` need not outlive it.
  void addNdjson(const AsyncTraceWriter& writer);

  bool empty() const {
    return listeners.empty() && writers.empty();
  }

 private:
  kj::Vector<kj::Own<AsyncTraceListener>> listeners;
  kj::Vector<const AsyncTraceWriter*> writers;
  friend class AsyncTracker;
};

// A handle to an async resource, held by whatever will later report on it (typically captured in
// a continuation). Move-only. A default-constructed AsyncResource, or one created while tracing is
// off, is inert: every method does nothing.
//
// The handle keeps its tracker alive, so it may outlive the IoContext; after the tracker closes,
// its methods do nothing.
class AsyncResource {
 public:
  AsyncResource() = default;
  AsyncResource(AsyncResource&& other) noexcept;
  AsyncResource& operator=(AsyncResource&& other) noexcept(false);
  KJ_DISALLOW_COPY(AsyncResource);

  // Destroying the handle tells the tracker that no more events will come for this resource.
  // If it never settled, a `destroy` event is reported.
  ~AsyncResource() noexcept(false);

  // 0 if inert.
  AsyncId getId() const {
    return id;
  }

  // The reporting methods are const: they change the tracker's record, not the handle.

  // The work behind the resource finished. Only the first settle is reported.
  void settle(AsyncOutcome outcome) const;

  // `key` and `value` need not be valid UTF-8; invalid sequences are replaced.
  void annotate(kj::StringPtr key, kj::StringPtr value) const;

  // Makes this resource the cause of the current turn. Its `before` is reported now and its `after`
  // when the turn ends (after the microtask drain).
  void enterAsTurnCause() const;

  // Takes the resource out of consideration for AsyncTracker::adoptOrCreate().
  void markBound() const;

  // Releases the resource early, as the destructor would. The handle becomes inert.
  void release();

 private:
  AsyncResource(kj::Arc<AsyncTracker> tracker, AsyncId id): tracker(kj::mv(tracker)), id(id) {}

  kj::Arc<AsyncTracker> tracker;
  AsyncId id = 0;

  friend class AsyncTracker;
};

// Tracks the async resources of one IoContext. Create with tryCreate(); the IoContext holds it
// through OwnedAsyncTracker.
//
// Methods are const and may be called from any thread, per KJ's convention. Calls made on a thread
// other than the one that created the tracker are dropped and reported in the context's stats
// (`foreignThread`).
class AsyncTracker final: public kj::AtomicRefcounted {
 public:
  // Returns none if `sinks` is empty. `actor` is the actor's ID, if this is an actor's context.
  static kj::Maybe<kj::Arc<AsyncTracker>> tryCreate(const AsyncTraceIsolate& isolate,
      AsyncTraceSinks&& sinks,
      kj::StringPtr worker,
      kj::Maybe<kj::StringPtr> actor);

  // Use tryCreate().
  AsyncTracker(::rust::Box<rust::async_trace::Tracker> impl,
      kj::Maybe<kj::Arc<const AsyncStackCapturer>> stackCapturer);
  ~AsyncTracker() noexcept(false);

  // Records a new resource, with its creation stack if the isolate captures stacks. A `trigger` of
  // 0 means the current turn's cause. Returns an inert handle if the resource was not recorded
  // (tracker closed, live-resource cap reached, or called from another thread). `name` need not
  // be valid UTF-8.
  AsyncResource create(AsyncKind kind, kj::StringPtr name, AsyncId trigger = 0) const;

  // The resource whose callback is running (`executionAsyncId`), or 0.
  AsyncId current() const;

  // Called by a KJ-to-JS bridge. Returns a handle to the binding operation that the bridge should
  // report under (see Tracker::adopt_operation in Rust), or else a new resource of `kind`. An
  // adopted operation stays known until both its creator and the bridge release it.
  AsyncResource adoptOrCreate(AsyncKind kind, kj::StringPtr name) const;

  // Takes `id` out of consideration for adoptOperation().
  void markBound(AsyncId id) const;

  // For the promise hook (async-trace-promises.h). A promise has no handle: the tracker forgets it
  // when it settles, or when the tracker closes. createPromise() returns 0 if it was not recorded.
  // `parent` is the promise it derives from, or 0 for the turn's cause. Promises never record a
  // creation stack; there are too many.
  AsyncId createPromise(AsyncId parent) const;
  // Whether `id` is a live resource of this tracker (a promise from another context isn't).
  bool knows(AsyncId id) const;
  void enterPromise(AsyncId id) const {
    enter(id);
  }
  void exitPromise(AsyncId id) const {
    exit(id);
  }
  void settlePromise(AsyncId id, AsyncOutcome outcome) const {
    settle(id, outcome);
  }

  // The tracker of the innermost turn on this thread (see TurnScope), if that turn is traced.
  // Lets code that cannot reach the IoContext, such as TraceContextParent::newChild(), find the
  // tracker. One thread-local read when tracing is off.
  static kj::Maybe<const AsyncTracker&> inTurn() {
    const AsyncTracker* tracker = _::trackerInTurn;
    if (tracker == nullptr) return kj::none;
    return *tracker;
  }

  // Ends tracking: reports the context's stats and destroys the sinks. Later calls do nothing.
  // Must be called on the tracker's thread; OwnedAsyncTracker does so.
  void close() const;

  // Brackets one entry into JavaScript from the event loop. Construct it before acquiring the
  // isolate lock and destroy it after the microtask drain. `defaultCause` (typically the current
  // request) is the turn's cause unless a resource calls enterAsTurnCause().
  class TurnScope {
   public:
    // Always records the turn's tracker (null if untraced) for inTurn(), so a nested untraced
    // context's operations are not attributed to an enclosing traced one.
    explicit TurnScope(kj::Maybe<const AsyncTracker&> tracker, AsyncId defaultCause = 0)
        : tracker(tracker),
          previousInTurn(_::trackerInTurn) {
      _::trackerInTurn = nullptr;
      KJ_IF_SOME(t, tracker) {
        _::trackerInTurn = &t;
        t.turnBegin(defaultCause);
      }
    }
    ~TurnScope() noexcept(false) {
      end();
    }
    KJ_DISALLOW_COPY_AND_MOVE(TurnScope);

    // The isolate lock is held; JavaScript can run.
    void locked() {
      KJ_IF_SOME(t, tracker) {
        t.turnLocked();
      }
    }

    // Ends the turn early, as the destructor would. Call it before releasing the isolate lock:
    // ending the turn closes its cause's callback scope, and sinks such as the inspector's must
    // see that while the lock is held.
    void end() {
      if (ended) return;
      ended = true;
      _::trackerInTurn = previousInTurn;
      KJ_IF_SOME(t, tracker) {
        t.turnEnd();
      }
    }

   private:
    kj::Maybe<const AsyncTracker&> tracker;
    const AsyncTracker* previousInTurn;
    bool ended = false;
  };

  // Attributes a callback to `resource` for the scope's lifetime (`before`/`after`). Use for
  // callbacks nested inside a turn, such as microtasks; a resource that starts a turn uses
  // enterAsTurnCause() instead.
  class CallbackScope {
   public:
    explicit CallbackScope(const AsyncResource& resource)
        : tracker(resource.tracker.get()),
          id(resource.id) {
      if (tracker != nullptr) {
        tracker->enter(id);
      }
    }
    ~CallbackScope() noexcept(false) {
      if (tracker != nullptr) {
        tracker->exit(id);
      }
    }
    KJ_DISALLOW_COPY_AND_MOVE(CallbackScope);

   private:
    // The resource's handle keeps the tracker alive for the scope's duration.
    const AsyncTracker* tracker;
    AsyncId id;
  };

 private:
  mutable ::rust::Box<rust::async_trace::Tracker> impl;
  kj::Maybe<kj::Arc<const AsyncStackCapturer>> stackCapturer;
  const void* ownerThread;
  mutable std::atomic<uint64_t> foreignThreadCalls{0};

  // Returns true on the owning thread; otherwise counts the call and returns false.
  bool onOwnerThread() const;

  void settle(AsyncId id, AsyncOutcome outcome) const;
  void annotate(AsyncId id, kj::StringPtr key, kj::StringPtr value) const;
  void destroy(AsyncId id) const;
  void enter(AsyncId id) const;
  void exit(AsyncId id) const;
  void setTurnCause(AsyncId id) const;
  void turnBegin(AsyncId defaultCause) const;
  void turnLocked() const;
  void turnEnd() const;

  friend class AsyncResource;
};

// The IoContext's reference to its tracker. Closes the tracker when destroyed, so the context's
// stats are reported even while AsyncResource handles keep the tracker itself alive.
class OwnedAsyncTracker {
 public:
  OwnedAsyncTracker() = default;
  explicit OwnedAsyncTracker(kj::Maybe<kj::Arc<AsyncTracker>> tracker);
  ~OwnedAsyncTracker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(OwnedAsyncTracker);

  kj::Maybe<const AsyncTracker&> get() const {
    if (tracker == nullptr) return kj::none;
    return *tracker;
  }

 private:
  kj::Arc<AsyncTracker> tracker;
};

// =======================================================================================
// AsyncResource inline implementation. Inline so that an inert handle costs one null check.

inline AsyncResource::AsyncResource(AsyncResource&& other) noexcept
    : tracker(kj::mv(other.tracker)),
      id(other.id) {
  other.id = 0;
}

inline AsyncResource& AsyncResource::operator=(AsyncResource&& other) noexcept(false) {
  if (this != &other) {
    release();
    tracker = kj::mv(other.tracker);
    id = other.id;
    other.id = 0;
  }
  return *this;
}

inline AsyncResource::~AsyncResource() noexcept(false) {
  release();
}

inline void AsyncResource::settle(AsyncOutcome outcome) const {
  if (tracker != nullptr) tracker->settle(id, outcome);
}

inline void AsyncResource::annotate(kj::StringPtr key, kj::StringPtr value) const {
  if (tracker != nullptr) tracker->annotate(id, key, value);
}

inline void AsyncResource::enterAsTurnCause() const {
  if (tracker != nullptr) tracker->setTurnCause(id);
}

inline void AsyncResource::markBound() const {
  if (tracker != nullptr) tracker->markBound(id);
}

inline void AsyncResource::release() {
  if (tracker != nullptr) {
    tracker->destroy(id);
    tracker = nullptr;
    id = 0;
  }
}

}  // namespace workerd
