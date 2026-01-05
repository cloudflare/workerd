// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// ======================================================================================
// DeferredPromise<T> - An Optimized Alternative to jsg::Promise<T>
//
// Sketch implementation provided entirely by Claude under supervision.
//
// Motivation:
// -----------
// jsg::Promise<T> always wraps a V8 JavaScript promise, even when the value is
// immediately available. This incurs several costs:
//
//   1. V8 promise allocation - Each promise requires a V8 heap object
//   2. Opaque wrapping - C++ values must be wrapped in OpaqueWrapper for V8
//   3. Microtask scheduling - Continuations run on V8's microtask queue, not
//      synchronously, even when values are immediately available
//
// DeferredPromise<T> avoids this overhead by storing state in pure C++ and
// deferring V8 promise creation until explicitly requested via toJsPromise().
// When a value is immediately available, continuations execute synchronously
// without any V8 involvement.
//
// Basic Usage:
// ------------
//   // Create a deferred promise/resolver pair
//   auto pair = newDeferredPromiseAndResolver<int>();
//
//   // Or via jsg::Lock for proper isolate context:
//   auto pair = js.newDeferredPromiseAndResolver<int>();
//
//   // Set up a chain of continuations
//   pair.promise.then(js, [](jsg::Lock&, int value) {
//     return value * 2;
//   }).then(js, [](jsg::Lock&, int doubled) {
//     KJ_LOG(INFO, "got doubled value", doubled);
//   });
//
//   // Resolve - all continuations run synchronously NOW
//   pair.resolver.resolve(js, 21);  // Logs "got doubled value 42"
//
// Single-Consumer Semantics:
// --------------------------
// Like kj::Promise, DeferredPromise uses single-consumer semantics. Calling
// .then() or .catch_() CONSUMES the promise - you cannot attach multiple
// independent consumers. This design avoids the complexity of fan-out.
//
//   auto pair = newDeferredPromiseAndResolver<int>();
//   pair.promise.then(js, ...);  // OK - consumes the promise
//   pair.promise.then(js, ...);  // ERROR - promise already consumed!
//
// Exception: whenResolved() does NOT consume the promise. It returns a new
// DeferredPromise<void> that settles when the original settles (propagates rejections):
//
//   pair.promise.whenResolved(js).then(js, ...);  // Does NOT consume
//   pair.promise.then(js, ...);  // Still works!
//
// When To Use:
// ------------
//   - Internal C++ code where promises often resolve synchronously
//   - Stream implementations where data is frequently immediately available
//   - Any hot path where jsg::Promise overhead is measurable
//   - When building chains of C++ callbacks that don't need JS visibility
//
// When To Use jsg::Promise<T> Instead:
// -------------------------------------
//   - When returning promises directly to JavaScript code
//   - When integrating with existing code that expects jsg::Promise<T>
//   - When you need full V8 promise semantics (microtask timing guarantees)
//   - When the promise needs to be observable from JavaScript
//   - When the JS promise needs to be preserved. The DeferredPromise does
//     not maintain a persistent reference to the V8 promise after fromJsPromise()
//
// API Reference:
// --------------
// DeferredPromise<T> mirrors jsg::Promise<T>'s API:
//
//   Continuation Methods (all consume the promise except whenResolved):
//     - then(func)              - Attach success continuation, returns new promise
//     - then(func, errorFunc)   - Attach success and error handlers
//     - catch_(errorFunc)       - Attach error handler only
//     - whenResolved()          - Get void promise that settles with original (NON-consuming)
//
//   State Queries:
//     - isPending()             - True if not yet resolved/rejected
//     - isResolved()            - True if resolved with a value
//     - isRejected()            - True if rejected with an error
//     - tryConsumeResolved()    - Get value if already resolved (CONSUMES promise)
//     - tryConsumeRejected()    - Get exception if already rejected (CONSUMES promise)
//
//   Conversion:
//     - toJsPromise(js)         - Convert to jsg::Promise (creates V8 promise)
//
//   Other:
//     - markAsHandled(js)       - Mark rejection as handled (prevents warnings)
//     - visitForGc(visitor)     - GC visitor integration
//
//   Resolver Methods:
//     - resolve(js, value)      - Resolve with a value (runs continuations)
//     - resolve(js)             - Resolve void promise
//     - reject(js, exception)   - Reject with kj::Exception (primary), V8 value, or jsg::Value
//     - addRef()                - Create another resolver sharing the same state
//
//   Factory Functions:
//     - newDeferredPromiseAndResolver<T>()  - Create promise/resolver pair
//     - DeferredPromise<T>::resolved(value) - Create already-resolved promise
//     - DeferredPromise<T>::rejected(js, e) - Create already-rejected promise
//     - DeferredPromise<T>::fromJsPromise() - Convert from jsg::Promise
//
// Error Handling:
// ---------------
// DeferredPromise stores rejections natively as kj::Exception to preserve async
// stack trace information. Error handlers receive kj::Exception directly:
//
//   promise.then(js,
//       [](Lock& js, int value) { return value * 2; },
//       [](Lock& js, kj::Exception exception) {
//         // Handle error - trace info preserved!
//         KJ_LOG(ERROR, exception);
//         return 0;  // Recovery value
//       });
//
//   promise.catch_(js, [](Lock& js, kj::Exception exception) {
//     // Exception propagated through chain with full trace
//     return 0;
//   });
//
// Benefits of kj::Exception storage:
//   - Async stack traces are preserved through the entire promise chain
//   - No JS allocation until toJsPromise() is called
//   - Efficient error propagation without V8 roundtrips
//
// Promise Chaining:
// -----------------
// Callbacks passed to .then() can return:
//
//   1. Plain values - Wrapped in a resolved DeferredPromise automatically
//   2. DeferredPromise<U> - Automatically unwrapped/chained (stays synchronous)
//   3. jsg::Promise<U> - Converted and chained (runs on microtask queue)
//
// Example:
//   pair.promise.then(js, [](jsg::Lock& js, int value) -> DeferredPromise<int> {
//     return someAsyncOperation(value);
//   }).then(js, [](jsg::Lock&, int finalValue) {
//     // Runs when the inner promise resolves
//   });
//
// Converting From jsg::Promise:
// -----------------------------
// Use fromJsPromise() to convert a jsg::Promise to DeferredPromise. This allows
// setting up an optimized chain of continuations that run synchronously when
// the jsg::Promise eventually resolves (via microtask):
//
//   auto jsPromise = someApiReturningJsPromise();
//   auto deferred = DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));
//
//   // These continuations run synchronously when jsPromise resolves
//   deferred.then(js, [](jsg::Lock&, int v) { return v * 2; })
//          .then(js, [](jsg::Lock&, int v) { return v + 10; })
//          .then(js, [](jsg::Lock&, int v) { /* final handling */ });
//
// TypeWrapper Integration:
// -------------------------
// DeferredPromise<T> integrates with the type wrapper system. When a jsg exposed
// method accepts a DeferredPromise<T>, and the value is a JS promise that is
// already resolved, the value is unwrapped synchronously without the need for
// an additional microtask hop. If the JS promise is rejected, the rejection is
// also propagated synchronously. If the JS promise is still pending, or if the
// value is a thenable, the full async conversion path via jsg::Promise<T> is used.
// Otherwise the value is unwrapped directly as already resolved.
//
// When a DeferredPromise<T> is returned to JavaScript, it is converted to a
// JS promise. If the DeferredPromise is already resolved or rejected, the JS promise
// is created in that state immediately. Otherwise, a pending JS promise is created and
// resolved/rejected when the DeferredPromise settles.
//
// Ownership Model:
// ----------------
// DeferredPromise and its Resolver share ownership of the underlying state via
// kj::Rc (non-atomic reference counting - safe because both must be used on
// the same thread/isolate). Either can be dropped first - the state lives until
// both are gone and all continuations have completed.
//
// State Machine:
// --------------
// The promise uses a state machine with four states:
//
//   Pending  - Initial state. Callbacks can be attached, waiting for resolution.
//   Resolved - Promise was resolved with a value. Callbacks run synchronously.
//   Rejected - Promise was rejected with an error. Error handlers run.
//   Consumed - Promise was consumed by .then()/.catch_()/toJsPromise().
//
// GC Integration:
// ---------------
// DeferredPromise properly integrates with JSG's garbage collection. Call
// visitForGc() to trace any JavaScript values held by the promise:
//
//   void visitForGc(GcVisitor& visitor) {
//     myDeferredPromise.visitForGc(visitor);
//   }
//
// ======================================================================================
// USAGE EXAMPLES
//
// The following examples demonstrate practical use cases where DeferredPromise
// provides significant performance benefits over jsg::Promise.
//
// -----------------------------------------------------------------------------
// Example 1: Buffered Stream Reader
// -----------------------------------------------------------------------------
//
// A stream that returns data immediately when buffered, but waits for I/O when
// the buffer is empty. This is the canonical DeferredPromise use case.
//
//   class BufferedReader {
//     kj::Vector<kj::byte> buffer;
//     kj::Maybe<DeferredPromiseResolver<kj::Array<kj::byte>>> pendingRead;
//
//    public:
//     // Called by consumer to read data
//     DeferredPromise<kj::Array<kj::byte>> read(Lock& js, size_t maxBytes) {
//       if (buffer.size() > 0) {
//         // Fast path: data available, return immediately (no V8 involvement!)
//         auto chunk = extractFromBuffer(maxBytes);
//         return DeferredPromise<kj::Array<kj::byte>>::resolved(kj::mv(chunk));
//       }
//
//       // Slow path: no data, need to wait for I/O
//       auto [promise, resolver] = newDeferredPromiseAndResolver<kj::Array<kj::byte>>();
//       pendingRead = kj::mv(resolver);
//       return kj::mv(promise);
//     }
//
//     // Called when I/O completes
//     void onDataReceived(Lock& js, kj::Array<kj::byte> data) {
//       KJ_IF_SOME(resolver, pendingRead) {
//         // Resolve the pending read - continuation runs synchronously
//         resolver.resolve(js, kj::mv(data));
//         pendingRead = kj::none;
//       } else {
//         buffer.addAll(data);
//       }
//     }
//   };
//
// -----------------------------------------------------------------------------
// Example 2: Cache with Async Fallback
// -----------------------------------------------------------------------------
//
// Cache hits return immediately; misses trigger async fetch.
//
//   class AsyncCache {
//     kj::HashMap<kj::String, CachedValue> cache;
//
//    public:
//     DeferredPromise<CachedValue> get(Lock& js, kj::StringPtr key) {
//       KJ_IF_SOME(value, cache.find(key)) {
//         // Cache hit - return immediately (very fast, no V8!)
//         return DeferredPromise<CachedValue>::resolved(value.clone());
//       }
//
//       // Cache miss - fetch asynchronously
//       auto [promise, resolver] = newDeferredPromiseAndResolver<CachedValue>();
//
//       fetchFromOrigin(key).then([resolver = kj::mv(resolver), key = kj::str(key)]
//                                 (CachedValue value) mutable {
//         cache.insert(kj::mv(key), value.clone());
//         resolver.resolve(js, kj::mv(value));
//       });
//
//       return kj::mv(promise);
//     }
//   };
//
// -----------------------------------------------------------------------------
// Example 3: Rate Limiter
// -----------------------------------------------------------------------------
//
// Returns immediately if under rate limit, waits if throttled.
//
//   class RateLimiter {
//     size_t tokensAvailable;
//     kj::Vector<DeferredPromiseResolver<void>> waiting;
//
//    public:
//     DeferredPromise<void> acquire(Lock& js) {
//       if (tokensAvailable > 0) {
//         --tokensAvailable;
//         return DeferredPromise<void>::resolved();  // Immediate!
//       }
//
//       // Need to wait for token
//       auto [promise, resolver] = newDeferredPromiseAndResolver<void>();
//       waiting.add(kj::mv(resolver));
//       return kj::mv(promise);
//     }
//
//     void release(Lock& js) {
//       if (waiting.size() > 0) {
//         auto resolver = kj::mv(waiting.front());
//         waiting.erase(waiting.begin(), waiting.begin() + 1);
//         resolver.resolve(js);  // Wake up next waiter
//       } else {
//         ++tokensAvailable;
//       }
//     }
//   };
//
// -----------------------------------------------------------------------------
// Example 4: Batching Multiple Operations
// -----------------------------------------------------------------------------
//
// Collect operations and batch them for efficiency.
//
//   class BatchProcessor {
//     struct PendingOp {
//       Request request;
//       DeferredPromiseResolver<Response> resolver;
//     };
//     kj::Vector<PendingOp> pending;
//     static constexpr size_t BATCH_SIZE = 100;
//
//    public:
//     DeferredPromise<Response> submit(Lock& js, Request request) {
//       auto [promise, resolver] = newDeferredPromiseAndResolver<Response>();
//       pending.add({kj::mv(request), kj::mv(resolver)});
//
//       if (pending.size() >= BATCH_SIZE) {
//         flush(js);
//       }
//
//       return kj::mv(promise);
//     }
//
//     void flush(Lock& js) {
//       auto batch = kj::mv(pending);
//       pending = {};
//
//       // Process batch asynchronously
//       processBatch(batch).then([batch = kj::mv(batch)](auto responses) {
//         for (size_t i = 0; i < batch.size(); ++i) {
//           batch[i].resolver.resolve(js, kj::mv(responses[i]));
//         }
//       });
//     }
//   };
//
// -----------------------------------------------------------------------------
// Example 5: Connection Pool
// -----------------------------------------------------------------------------
//
// Return available connection immediately, wait if pool exhausted.
//
//   class ConnectionPool {
//     kj::Vector<kj::Own<Connection>> available;
//     kj::Vector<DeferredPromiseResolver<kj::Own<Connection>>> waiters;
//
//    public:
//     DeferredPromise<kj::Own<Connection>> acquire(Lock& js) {
//       if (available.size() > 0) {
//         auto conn = kj::mv(available.back());
//         available.removeLast();
//         return DeferredPromise<kj::Own<Connection>>::resolved(kj::mv(conn));
//       }
//
//       auto [promise, resolver] = newDeferredPromiseAndResolver<kj::Own<Connection>>();
//       waiters.add(kj::mv(resolver));
//       return kj::mv(promise);
//     }
//
//     void release(Lock& js, kj::Own<Connection> conn) {
//       if (waiters.size() > 0) {
//         auto resolver = kj::mv(waiters.front());
//         waiters.erase(waiters.begin(), waiters.begin() + 1);
//         resolver.resolve(js, kj::mv(conn));
//       } else {
//         available.add(kj::mv(conn));
//       }
//     }
//   };
//
// -----------------------------------------------------------------------------
// Example 6: Lazy Initialization
// -----------------------------------------------------------------------------
//
// Initialize resource on first access, share result with concurrent callers.
// Since DeferredPromise has single-consumer semantics, we store resolvers for
// all pending callers rather than sharing a single promise.
//
//   class LazyResource {
//     kj::Maybe<Resource> cached;
//     kj::Vector<DeferredPromiseResolver<Resource>> pendingResolvers;
//     bool initStarted = false;
//
//    public:
//     DeferredPromise<Resource> get(Lock& js) {
//       KJ_IF_SOME(resource, cached) {
//         return DeferredPromise<Resource>::resolved(resource.clone());
//       }
//
//       // Create a new promise/resolver pair for this caller
//       auto [promise, resolver] = newDeferredPromiseAndResolver<Resource>();
//       pendingResolvers.add(kj::mv(resolver));
//
//       if (!initStarted) {
//         initStarted = true;
//         initializeAsync().then([this](Lock& js, Resource r) {
//           cached = kj::mv(r);
//           // Resolve all pending callers
//           for (auto& resolver : pendingResolvers) {
//             resolver.resolve(js, KJ_ASSERT_NONNULL(cached).clone());
//           }
//           pendingResolvers.clear();
//         });
//       }
//
//       return kj::mv(promise);
//     }
//   };
//
// -----------------------------------------------------------------------------
// Example 7: Converting jsg::Promise Chain to Synchronous
// -----------------------------------------------------------------------------
//
// When receiving a jsg::Promise from external code, convert to DeferredPromise
// to make the continuation chain run synchronously.
//
//   void processExternalPromise(Lock& js, jsg::Promise<Data> externalPromise) {
//     // Convert to DeferredPromise - continuations will run synchronously
//     // once the external promise resolves (via microtask)
//     auto deferred = DeferredPromise<Data>::fromJsPromise(js, kj::mv(externalPromise));
//
//     // This entire chain runs synchronously after the microtask
//     deferred
//         .then(js, [](Lock&, Data d) { return validate(d); })
//         .then(js, [](Lock&, Data d) { return transform(d); })
//         .then(js, [](Lock&, Data d) { return compress(d); })
//         .then(js, [](Lock&, Data d) { store(d); });
//   }
//
// ======================================================================================

// clang-format off
// Order matters: jsg.h must come before function.h to avoid circular dependency
// where modules.h tries to instantiate jsg::Function before it's defined.
#include "jsg.h"
#include "function.h"
#include "promise.h"
// clang-format on

#include <workerd/util/state-machine.h>

#include <kj/vector.h>

// Helper to capture the return address for async stack traces.
// This captures the address of the code that called the current function,
// which is useful for building async stack traces through promise chains.
#if __GNUC__
#define JSG_GET_RETURN_ADDRESS() __builtin_return_address(0)
#elif _MSC_VER
#define JSG_GET_RETURN_ADDRESS() _ReturnAddress()
#else
#define JSG_GET_RETURN_ADDRESS() nullptr
#endif

namespace workerd::jsg {

// Forward declarations
template <typename T>
class DeferredPromise;

template <typename T>
class DeferredPromiseResolver;

// Note: DeferredPromiseResolverPair and newDeferredPromiseAndResolver are forward-declared
// in jsg.h, which is included above.

template <typename T>
concept VoidType = isVoid<T>();

template <typename T>
concept NonVoidType = !isVoid<T>();

// Detect if T is a DeferredPromise<U> for some U.
// Used to detect when a callback returns a DeferredPromise for automatic chaining.
template <typename T>
constexpr bool kIsDeferredPromise = false;
template <typename T>
constexpr bool kIsDeferredPromise<DeferredPromise<T>> = true;

// Detect if T is a jsg::Promise<U> for some U.
// Used to detect when a callback returns a jsg::Promise for automatic chaining.
template <typename T>
constexpr bool kIsJsgPromiseType = false;
template <typename T>
constexpr bool kIsJsgPromiseType<Promise<T>> = true;

// Detect if T is any promise type (DeferredPromise or jsg::Promise).
// Useful for generic code that handles both promise types.
template <typename T>
constexpr bool kIsAnyPromise = kIsDeferredPromise<T> || kIsJsgPromiseType<T>;

// Extract the inner type from DeferredPromise<T> or jsg::Promise<T>.
// For non-promise types, returns the type unchanged.
// Example: RemoveAnyPromise<DeferredPromise<int>> -> int
//          RemoveAnyPromise<jsg::Promise<void>> -> void
//          RemoveAnyPromise<int> -> int
template <typename T>
struct RemoveAnyPromise_ {
  using Type = T;
};
template <typename T>
struct RemoveAnyPromise_<DeferredPromise<T>> {
  using Type = T;
};
template <typename T>
struct RemoveAnyPromise_<Promise<T>> {
  using Type = T;
};
template <typename T>
using RemoveAnyPromise = typename RemoveAnyPromise_<T>::Type;

// Alias for backwards compatibility.
template <typename T>
using RemoveDeferredPromise = RemoveAnyPromise<T>;

namespace _ {  // private

// ======================================================================================
// Continuation Trampoline
// ======================================================================================
//
// To avoid stack overflow with deep promise chains, we use a trampolining pattern.
// Instead of directly invoking callbacks (which would nest stack frames), we push
// them onto a queue. Only the outermost resolve() call drains the queue in a loop,
// keeping stack depth O(1) regardless of chain length.
//
// This maintains synchronous execution semantics (all callbacks complete before
// the outermost resolve() returns) while avoiding stack overflow.

class ContinuationQueue {
 public:
  // Schedule a continuation to run. If we're already draining, it gets queued.
  // If not, we execute it directly (fast path) and drain any subsequently queued work.
  // Note: Uses kj::Function rather than jsg::Function because this is a thread-local
  // static queue, and jsg::Function's Wrappable destruction semantics require proper
  // context that isn't available when the thread-local is destroyed.
  inline void schedule(Lock& js, kj::Function<void(Lock&)> continuation) {
    if (draining) {
      // Already draining - queue for later processing
      queue.add(kj::mv(continuation));
    } else {
      // Fast path: execute immediately without touching the queue
      draining = true;
      KJ_DEFER({
        draining = false;
        // Only clear if we actually used the queue
        if (drainIndex > 0) {
          queue.clear();
          drainIndex = 0;
        }
      });

      // Execute the continuation directly (avoids queue.add() overhead)
      continuation(js);

      // Drain any continuations that were queued during execution
      while (drainIndex < queue.size()) {
        auto next = kj::mv(queue[drainIndex]);
        ++drainIndex;
        next(js);
      }
    }
  }

  // Check if we're currently draining (i.e., inside a resolve() call chain)
  inline bool isDraining() const {
    return draining;
  }

 private:
  kj::Vector<kj::Function<void(Lock&)>> queue;
  size_t drainIndex = 0;
  bool draining = false;
};

// Thread-local continuation queue.
//
// Thread-local is safe here because:
// 1. DeferredPromise must be used on a single thread (the one owning the jsg::Lock)
// 2. Continuations are drained synchronously - by the time schedule() returns (for
//    the outermost call), the queue is always empty. No continuations persist across
//    separate resolve operations, so nothing is left dangling in thread-local storage.
inline ContinuationQueue& getContinuationQueue() {
  static thread_local ContinuationQueue queue;
  return queue;
}

// ======================================================================================
// State types for the state machine

template <typename T>
struct DeferredPending;

template <typename T>
struct DeferredResolved;

struct DeferredRejected;

// ======================================================================================
// Continuation types - type-erased callbacks

// A continuation that receives the resolved value
template <typename T>
struct ThenCallbackType {
  using Type = Function<void(T)>;
};

template <>
struct ThenCallbackType<void> {
  using Type = Function<void()>;
};

template <typename T>
using ThenCallback = typename ThenCallbackType<T>::Type;

// A continuation that receives the rejection reason as a kj::Exception.
// We store exceptions natively to preserve async trace information and defer
// JS conversion until actually needed (e.g., when converting to jsg::Promise).
using CatchCallback = Function<void(kj::Exception)>;

// ======================================================================================
// Tag types for direct state construction (avoids creating Pending then transitioning)
struct DirectResolvedTag {};
struct DirectRejectedTag {};

// Shared state owned by both DeferredPromise and Resolver via kj::Rc

template <typename T>
class DeferredPromiseState final: public kj::Refcounted {
 public:
  // State types
  struct Pending {
    static constexpr kj::StringPtr NAME = "pending"_kj;

    // Single continuation - .then() consumes the promise like kj::Promise
    kj::Maybe<ThenCallback<T>> thenCallback;
    kj::Maybe<CatchCallback> catchCallback;

    // Resolution observers - called when promise settles, don't consume the promise.
    // Used by whenResolved() to observe without taking ownership.
    // Receives kj::none on success, or the exception on rejection.
    kj::Vector<Function<void(kj::Maybe<kj::Exception>)>> resolutionObservers;

    // If converted to jsg::Promise, we keep the resolver to forward resolution
    kj::Maybe<typename Promise<T>::Resolver> jsResolver;
  };

  struct Resolved {
    static constexpr kj::StringPtr NAME = "resolved"_kj;
    T value;
    explicit Resolved(T&& v): value(kj::mv(v)) {}
  };

  struct Rejected {
    static constexpr kj::StringPtr NAME = "rejected"_kj;
    kj::Exception exception;
    explicit Rejected(kj::Exception&& e): exception(kj::mv(e)) {}
  };

  // Consumed state - promise was moved away via .then() or similar
  struct Consumed {
    static constexpr kj::StringPtr NAME = "consumed"_kj;
  };

  // State machine configuration:
  // - Consumed is the only terminal state (promise can never be used after consumption)
  // - Rejected is the error state (enables isErrored() API)
  //   Note: ErrorState makes Rejected implicitly terminal, so transitions from
  //   Rejected→Consumed require forceTransitionTo (this is the intended pattern
  //   per StateMachine docs for "cleanup/reset" scenarios)
  // - Pending is the active state (enables isActive(), whenActive() APIs)
  using State = workerd::StateMachine<workerd::TerminalStates<Consumed>,
      workerd::ErrorState<Rejected>,
      workerd::ActiveState<Pending>,
      Pending,
      Resolved,
      Rejected,
      Consumed>;

  // Default constructor creates pending state
  DeferredPromiseState() = default;

  // Direct construction in Resolved state (avoids Pending allocation + transition)
  explicit DeferredPromiseState(DirectResolvedTag, T&& value)
      : state(State::template create<Resolved>(kj::mv(value))) {}

  // Direct construction in Rejected state (avoids Pending allocation + transition)
  explicit DeferredPromiseState(DirectRejectedTag, kj::Exception&& exception)
      : state(State::template create<Rejected>(kj::mv(exception))) {}

  State state = State::template create<Pending>();
  bool markedAsHandled = false;

  // Resolve with a value
  void resolve(Lock& js, T value) {
    KJ_IF_SOME(pending, state.template tryGetUnsafe<Pending>()) {
      // Notify resolution observers first (they don't consume the value)
      // Observers are scheduled via trampoline to avoid stack buildup
      auto observers = kj::mv(pending.resolutionObservers);
      for (auto& observer: observers) {
        getContinuationQueue().schedule(
            js, [obs = kj::mv(observer)](Lock& js) mutable { obs(js, kj::none); });
      }

      // Notify JS resolver if one exists - value is forwarded to JS, go directly to Consumed
      // (jsResolver and thenCallback are mutually exclusive, verified in toJsPromise())
      KJ_IF_SOME(resolver, pending.jsResolver) {
        resolver.resolve(js, kj::mv(value));
        state.template transitionTo<Consumed>();
        return;
      }

      // Schedule the continuation via trampoline to avoid stack buildup
      auto callback = kj::mv(pending.thenCallback);

      KJ_IF_SOME(cb, callback) {
        // Pass value directly to continuation, skip storing in Resolved state
        state.template transitionTo<Consumed>();
        getContinuationQueue().schedule(
            js, [c = kj::mv(cb), v = kj::mv(value)](Lock& js) mutable { c(js, kj::mv(v)); });
      } else {
        // No callback - store value in Resolved state for later consumption
        state.template transitionTo<Resolved>(kj::mv(value));
      }
    }
  }

  // Reject with an exception - this is the primary rejection method.
  // The exception is stored natively to preserve async trace information.
  void reject(Lock& js, kj::Exception&& exception) {
    KJ_IF_SOME(pending, state.template tryGetUnsafe<Pending>()) {
      // Notify resolution observers first via trampoline
      // Each observer receives a copy of the exception to propagate rejections
      auto observers = kj::mv(pending.resolutionObservers);
      for (auto& observer: observers) {
        // Copying the exception is intentional to keep things simple. It is not expected
        // that there will be many observers in the typical case. At some hypothetical future
        // point we could optimize by sharing the exception in a refcounted wrapper if needed
        // but copying kj::Exception here is cheap enough for now.
        getContinuationQueue().schedule(
            js, [obs = kj::mv(observer), e = kj::cp(exception)](Lock& js) mutable {
          obs(js, kj::mv(e));
        });
      }

      // Notify JS resolver if one exists - convert to JS and forward
      // (jsResolver and catchCallback are mutually exclusive, verified in toJsPromise())
      KJ_IF_SOME(resolver, pending.jsResolver) {
        resolver.reject(js, js.exceptionToJs(kj::cp(exception)).getHandle(js));
        // Note: forceTransitionTo needed because we're going Pending→Consumed, skipping Rejected
        state.template forceTransitionTo<Consumed>();
        return;
      }

      // Schedule the catch callback via trampoline
      auto callback = kj::mv(pending.catchCallback);

      KJ_IF_SOME(cb, callback) {
        // Pass exception directly to continuation, skip storing in Rejected state
        // Note: forceTransitionTo needed because we're going Pending→Consumed, skipping Rejected
        state.template forceTransitionTo<Consumed>();
        getContinuationQueue().schedule(
            js, [c = kj::mv(cb), e = kj::mv(exception)](Lock& js) mutable { c(js, kj::mv(e)); });
      } else {
        // No callback - store exception in Rejected state for later consumption
        state.template transitionTo<Rejected>(kj::mv(exception));
      }
    }
  }

  // Reject with a JS value - converts to kj::Exception for internal storage
  void reject(Lock& js, Value error) {
    reject(js, js.exceptionToKj(kj::mv(error)));
  }

  inline bool isPending() const {
    return state.template is<Pending>();
  }
  inline bool isResolved() const {
    return state.template is<Resolved>();
  }
  inline bool isRejected() const {
    return state.isErrored();
  }
  inline bool isConsumed() const {
    return state.template is<Consumed>();
  }

  void visitForGc(GcVisitor& visitor) {
    state.visitForGc(visitor);
  }
};

// Specialization for void
template <>
class DeferredPromiseState<void> final: public kj::Refcounted {
 public:
  struct Pending {
    static constexpr kj::StringPtr NAME = "pending"_kj;
    kj::Maybe<ThenCallback<void>> thenCallback;
    kj::Maybe<CatchCallback> catchCallback;
    // Resolution observers - receives kj::none on success, or the exception on rejection.
    kj::Vector<Function<void(kj::Maybe<kj::Exception>)>> resolutionObservers;
    kj::Maybe<Promise<void>::Resolver> jsResolver;
  };

  struct Resolved {
    static constexpr kj::StringPtr NAME = "resolved"_kj;
  };

  struct Rejected {
    static constexpr kj::StringPtr NAME = "rejected"_kj;
    kj::Exception exception;
    explicit Rejected(kj::Exception&& e): exception(kj::mv(e)) {}
  };

  struct Consumed {
    static constexpr kj::StringPtr NAME = "consumed"_kj;
  };

  // State machine configuration (same as non-void DeferredPromiseState<T>):
  // - Consumed is the only terminal state (promise can never be used after consumption)
  // - Rejected is the error state (enables isErrored() API)
  //   Note: ErrorState makes Rejected implicitly terminal, so transitions from
  //   Rejected→Consumed require forceTransitionTo
  // - Pending is the active state (enables isActive(), whenActive() APIs)
  using State = workerd::StateMachine<workerd::TerminalStates<Consumed>,
      workerd::ErrorState<Rejected>,
      workerd::ActiveState<Pending>,
      Pending,
      Resolved,
      Rejected,
      Consumed>;

  // Default constructor creates pending state
  DeferredPromiseState() = default;

  // Direct construction in Resolved state (avoids Pending allocation + transition)
  explicit DeferredPromiseState(DirectResolvedTag): state(State::template create<Resolved>()) {}

  // Direct construction in Rejected state (avoids Pending allocation + transition)
  explicit DeferredPromiseState(DirectRejectedTag, kj::Exception&& exception)
      : state(State::template create<Rejected>(kj::mv(exception))) {}

  State state = State::template create<Pending>();
  bool markedAsHandled = false;

  void resolve(Lock& js) {
    KJ_IF_SOME(pending, state.template tryGetUnsafe<Pending>()) {
      // Notify resolution observers via trampoline
      auto observers = kj::mv(pending.resolutionObservers);
      for (auto& observer: observers) {
        getContinuationQueue().schedule(
            js, [obs = kj::mv(observer)](Lock& js) mutable { obs(js, kj::none); });
      }

      // Notify JS resolver if one exists - go directly to Consumed
      KJ_IF_SOME(resolver, pending.jsResolver) {
        resolver.resolve(js);
        state.transitionTo<Consumed>();
        return;
      }

      auto callback = kj::mv(pending.thenCallback);

      KJ_IF_SOME(cb, callback) {
        // Go directly to Consumed when callback exists
        state.transitionTo<Consumed>();
        getContinuationQueue().schedule(js, [c = kj::mv(cb)](Lock& js) mutable { c(js); });
      } else {
        // No callback - go to Resolved state for later consumption
        state.transitionTo<Resolved>();
      }
    }
  }

  // Reject with an exception - this is the primary rejection method.
  void reject(Lock& js, kj::Exception&& exception) {
    KJ_IF_SOME(pending, state.tryGetUnsafe<Pending>()) {
      // Notify resolution observers via trampoline
      // Each observer receives a copy of the exception to propagate rejections
      auto observers = kj::mv(pending.resolutionObservers);
      for (auto& observer: observers) {
        getContinuationQueue().schedule(
            js, [obs = kj::mv(observer), e = kj::cp(exception)](Lock& js) mutable {
          obs(js, kj::mv(e));
        });
      }

      // Notify JS resolver if one exists - convert to JS and forward
      KJ_IF_SOME(resolver, pending.jsResolver) {
        resolver.reject(js, js.exceptionToJs(kj::cp(exception)).getHandle(js));
        state.template forceTransitionTo<Consumed>();
        return;
      }

      auto callback = kj::mv(pending.catchCallback);

      KJ_IF_SOME(cb, callback) {
        // Pass exception directly to continuation, skip storing in Rejected state
        state.template forceTransitionTo<Consumed>();
        getContinuationQueue().schedule(
            js, [c = kj::mv(cb), e = kj::mv(exception)](Lock& js) mutable { c(js, kj::mv(e)); });
      } else {
        // No callback - store exception in Rejected state for later consumption
        state.template transitionTo<Rejected>(kj::mv(exception));
      }
    }
  }

  // Reject with a JS value - converts to kj::Exception for internal storage
  void reject(Lock& js, Value error) {
    reject(js, js.exceptionToKj(kj::mv(error)));
  }

  inline bool isPending() const {
    return state.template is<Pending>();
  }
  inline bool isResolved() const {
    return state.template is<Resolved>();
  }
  inline bool isRejected() const {
    return state.isErrored();
  }
  inline bool isConsumed() const {
    return state.template is<Consumed>();
  }

  void visitForGc(GcVisitor& visitor) {
    state.visitForGc(visitor);
  }
};

}  // namespace _

// ======================================================================================
// DeferredPromiseResolver<T>
//
// The resolver half of a DeferredPromise. Used to resolve or reject the
// associated promise. The resolver shares ownership of the promise state
// with the DeferredPromise - either can be dropped first.
//
// Usage:
//   auto pair = newDeferredPromiseAndResolver<int>();
//   // ... pass pair.promise to consumer ...
//   pair.resolver.resolve(js, 42);  // Runs all attached continuations
//
// Multiple resolvers can share the same state via addRef():
//   auto resolver2 = pair.resolver.addRef();
//   resolver2.resolve(js, 42);  // Same effect as pair.resolver.resolve()
//
// Only the first resolve/reject call has any effect - subsequent calls are
// silently ignored (the promise is already settled).

template <typename T>
class DeferredPromiseResolver {
 public:
  // Resolve the promise with a value.
  // For non-void promises, takes the value to resolve with.
  // Runs all attached continuations synchronously.
  // Has no effect if the promise is already resolved or rejected.
  template <typename U = T>
    requires NonVoidType<U>
  void resolve(Lock& js, U&& value) {
    state->resolve(js, kj::mv(value));
  }

  // Resolve a void promise.
  // Runs all attached continuations synchronously.
  // Has no effect if the promise is already resolved or rejected.
  void resolve(Lock& js)
    requires VoidType<T>
  {
    state->resolve(js);
  }

  // Resolve with another DeferredPromise - chains the promises.
  // When the inner promise settles, this promise settles with the same result.
  // Has no effect if this promise is already resolved or rejected.
  void resolve(Lock& js, DeferredPromise<T>&& promise) {
    // If we're not pending, nothing to do
    if (!state->isPending()) return;

    // Fast path: if inner promise is already rejected, reject immediately
    KJ_IF_SOME(exception, promise.tryConsumeRejected()) {
      state->reject(js, kj::mv(exception));
      return;
    }

    // Fast path: if inner promise is already resolved, resolve immediately
    if constexpr (isVoid<T>()) {
      if (promise.isResolved()) {
        // Consume it by transitioning to consumed state
        promise.state->state.template transitionTo<typename _::DeferredPromiseState<T>::Consumed>();
        state->resolve(js);
        return;
      }
    } else {
      KJ_IF_SOME(value, promise.tryConsumeResolved()) {
        state->resolve(js, kj::mv(value));
        return;
      }
    }

    // Inner promise is pending - chain by attaching continuations
    if constexpr (isVoid<T>()) {
      promise.then(js, [s = state.addRef()](Lock& js) mutable { s->resolve(js); },
          [s = state.addRef()](
              Lock& js, kj::Exception exception) mutable { s->reject(js, kj::mv(exception)); });
    } else {
      promise.then(js, [s = state.addRef()](Lock& js, T value) mutable {
        s->resolve(js, kj::mv(value));
      }, [s = state.addRef()](Lock& js, kj::Exception exception) mutable {
        s->reject(js, kj::mv(exception));
      });
    }
  }

  // Resolve with a jsg::Promise - chains the promises.
  // When the JS promise settles, this promise settles with the same result.
  // Has no effect if this promise is already resolved or rejected.
  void resolve(Lock& js, Promise<T>&& promise) {
    // If we're not pending, nothing to do
    if (!state->isPending()) return;

    // Fast path: check if already settled
    KJ_IF_SOME(settled, promise.tryConsumeSettled(js)) {
      if constexpr (isVoid<T>()) {
        KJ_SWITCH_ONEOF(settled) {
          KJ_CASE_ONEOF(resolved, typename Promise<T>::Resolved) {
            state->resolve(js);
          }
          KJ_CASE_ONEOF(error, Value) {
            state->reject(js, kj::mv(error));
          }
        }
      } else {
        KJ_SWITCH_ONEOF(settled) {
          KJ_CASE_ONEOF(value, T) {
            state->resolve(js, kj::mv(value));
          }
          KJ_CASE_ONEOF(error, Value) {
            state->reject(js, kj::mv(error));
          }
        }
      }
      return;
    }

    // JS promise is pending - chain by attaching continuations
    // Note: jsg::Promise error handlers receive Value, not kj::Exception
    if constexpr (isVoid<T>()) {
      promise.then(js, [s = state.addRef()](Lock& js) mutable { s->resolve(js); },
          [s = state.addRef()](Lock& js, Value error) mutable { s->reject(js, kj::mv(error)); });
    } else {
      promise.then(js, [s = state.addRef()](Lock& js, T value) mutable {
        s->resolve(js, kj::mv(value));
      }, [s = state.addRef()](Lock& js, Value error) mutable { s->reject(js, kj::mv(error)); });
    }
  }

  // Reject the promise with a kj::Exception.
  // The exception is stored natively to preserve async trace information.
  // Runs all attached error handlers synchronously.
  // Has no effect if the promise is already resolved or rejected.
  void reject(Lock& js, kj::Exception&& exception) {
    state->reject(js, kj::mv(exception));
  }

  // Reject the promise with a JavaScript exception value (converts to kj::Exception).
  // Has no effect if the promise is already resolved or rejected.
  void reject(Lock& js, v8::Local<v8::Value> error) {
    state->reject(js, Value(js.v8Isolate, error));
  }

  // Reject the promise with a jsg::Value (converts to kj::Exception).
  // Has no effect if the promise is already resolved or rejected.
  void reject(Lock& js, Value&& error) {
    state->reject(js, kj::mv(error));
  }

  // Create another resolver that shares the same promise state.
  // Useful when multiple code paths might resolve/reject the promise.
  // Only the first resolution/rejection takes effect.
  DeferredPromiseResolver addRef() {
    return DeferredPromiseResolver(state.addRef());
  }

  void visitForGc(GcVisitor& visitor) {
    state->visitForGc(visitor);
  }

  JSG_MEMORY_INFO(DeferredPromiseResolver) {
    tracker.trackField("state", state);
  }

 private:
  template <typename U>
  friend class DeferredPromise;

  template <typename U>
  friend DeferredPromiseResolverPair<U> newDeferredPromiseAndResolver();

  kj::Rc<_::DeferredPromiseState<T>> state;

  explicit DeferredPromiseResolver(kj::Rc<_::DeferredPromiseState<T>> s): state(kj::mv(s)) {}
};

// ======================================================================================
// DeferredPromise<T>
//
// The promise half of a deferred promise pair. Represents a value that may
// be available now or in the future. Consumers attach continuations via
// .then() or .catch_() to process the value when it becomes available.
//
// Key behaviors:
//   - If already resolved: continuations run synchronously when attached
//   - If pending: continuations are stored and run when resolved
//   - Single-consumer: .then()/.catch_() consume the promise (can only call once)
//   - Exception: whenResolved() does NOT consume (can still call .then() after)
//
// See file header for full documentation.

template <typename T>
class DeferredPromise {
 public:
  using Resolver = DeferredPromiseResolver<T>;

  // ======================================================================================
  // Factory Methods
  // Static methods for creating DeferredPromise instances in various states.

  // Create an already-resolved promise with the given value.
  // Continuations attached via .then() will run synchronously.
  // Uses direct state construction to avoid creating Pending state + transition.
  template <typename U = T>
    requires NonVoidType<U>
  static DeferredPromise resolved(U&& value) {
    return DeferredPromise(
        kj::rc<_::DeferredPromiseState<T>>(_::DirectResolvedTag{}, kj::mv(value)));
  }

  // Create an already-resolved void promise
  // Uses direct state construction to avoid creating Pending state + transition.
  static DeferredPromise resolved()
    requires VoidType<T>
  {
    return DeferredPromise(kj::rc<_::DeferredPromiseState<void>>(_::DirectResolvedTag{}));
  }

  // Create an already-rejected promise with a kj::Exception.
  // Uses direct state construction to avoid creating Pending state + transition.
  // This is the primary factory - stores exception natively for trace preservation.
  static DeferredPromise rejected(Lock& js, kj::Exception&& exception) {
    return DeferredPromise(
        kj::rc<_::DeferredPromiseState<T>>(_::DirectRejectedTag{}, kj::mv(exception)));
  }

  // Create an already-rejected promise from a JS value (converts to kj::Exception)
  static DeferredPromise rejected(Lock& js, v8::Local<v8::Value> error) {
    return rejected(js, js.exceptionToKj(Value(js.v8Isolate, error)));
  }

  static DeferredPromise rejected(Lock& js, Value&& error) {
    return rejected(js, js.exceptionToKj(kj::mv(error)));
  }

  // Create a DeferredPromise from a jsg::Promise.
  // This allows setting up an optimized chain of continuations on the
  // DeferredPromise that will run synchronously when the jsg::Promise resolves.
  //
  // If the jsg::Promise is already settled (resolved or rejected), the DeferredPromise
  // will be created in the corresponding settled state immediately, avoiding the
  // microtask queue delay.
  //
  // Usage:
  //   auto deferred = DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));
  //   deferred.then(js, [](Lock& js, int value) {
  //     // This runs synchronously when jsPromise resolves
  //   });
  static DeferredPromise fromJsPromise(Lock& js, Promise<T>&& promise) {
    // Optimization: If the promise is already settled, create a settled DeferredPromise
    // immediately without waiting for the microtask queue. Uses tryConsumeSettled() to
    // check state only once.
    KJ_IF_SOME(settled, promise.tryConsumeSettled(js)) {
      if constexpr (isVoid<T>()) {
        KJ_SWITCH_ONEOF(settled) {
          KJ_CASE_ONEOF(resolved, typename Promise<T>::Resolved) {
            return DeferredPromise::resolved();
          }
          KJ_CASE_ONEOF(error, Value) {
            return DeferredPromise::rejected(js, kj::mv(error));
          }
        }
      } else {
        KJ_SWITCH_ONEOF(settled) {
          KJ_CASE_ONEOF(value, T) {
            return DeferredPromise::resolved(kj::mv(value));
          }
          KJ_CASE_ONEOF(error, Value) {
            return DeferredPromise::rejected(js, kj::mv(error));
          }
        }
      }
      KJ_UNREACHABLE;
    }

    // Promise is pending - attach continuations that will resolve/reject when it settles
    auto state = kj::rc<_::DeferredPromiseState<T>>();
    auto returnState = state.addRef();

    if constexpr (isVoid<T>()) {
      promise.then(js, [state = kj::mv(state)](Lock& js) mutable { state->resolve(js); },
          [state2 = returnState.addRef()](
              Lock& js, Value error) mutable { state2->reject(js, kj::mv(error)); });
    } else {
      promise.then(js, [state = kj::mv(state)](Lock& js, T value) mutable {
        state->resolve(js, kj::mv(value));
      }, [state2 = returnState.addRef()](Lock& js, Value error) mutable {
        state2->reject(js, kj::mv(error));
      });
    }

    return DeferredPromise(kj::mv(returnState));
  }

  // ======================================================================================
  // Constructors

  DeferredPromise(DeferredPromise&&) = default;
  DeferredPromise& operator=(DeferredPromise&&) = default;
  KJ_DISALLOW_COPY(DeferredPromise);

  // ======================================================================================
  // Promise API - Continuation Methods
  // These methods attach callbacks that run when the promise settles.
  // IMPORTANT: All methods except whenResolved() CONSUME the promise -
  // you can only call one of then/catch_/toJsPromise per promise instance.

  // Mark the promise rejection as handled, preventing unhandled rejection warnings.
  // Should be called if you're intentionally ignoring a potential rejection.
  void markAsHandled(Lock& js) {
    state->markedAsHandled = true;
  }

  // Attach a success continuation and an error handler.
  // CONSUMES the promise - cannot call .then() again on the same promise.
  //
  // The callback receives (Lock& js, T value) and can return:
  //   - A plain value U -> returns DeferredPromise<U>
  //   - DeferredPromise<U> -> automatically chained, returns DeferredPromise<U>
  //   - jsg::Promise<U> -> automatically chained, returns DeferredPromise<U>
  //   - void -> returns DeferredPromise<void>
  //
  // The error handler receives (Lock& js, kj::Exception exception) and must return
  // the same type as the success callback.
  template <typename Func, typename ErrorFunc>
  auto then(Lock& js, Func&& func, ErrorFunc&& errorFunc)
      -> DeferredPromise<RemoveDeferredPromise<RemovePromise<ReturnType<Func, T, true>>>> {
    using ActualOutput = ReturnType<Func, T, true>;
    using Output = RemoveDeferredPromise<RemovePromise<ActualOutput>>;
    static_assert(
        kj::isSameType<Output,
            RemoveDeferredPromise<RemovePromise<ReturnType<ErrorFunc, kj::Exception, true>>>>(),
        "functions passed to .then() must return exactly the same type");

    return thenImpl<ActualOutput, Output>(js, kj::fwd<Func>(func), kj::fwd<ErrorFunc>(errorFunc));
  }

  // Attach a success continuation only; errors propagate to the returned promise.
  // CONSUMES the promise - cannot call .then() again on the same promise.
  // See two-argument then() for callback signature details.
  template <typename Func>
  auto then(Lock& js, Func&& func)
      -> DeferredPromise<RemoveDeferredPromise<RemovePromise<ReturnType<Func, T, true>>>> {
    using ActualOutput = ReturnType<Func, T, true>;
    using Output = RemoveDeferredPromise<RemovePromise<ActualOutput>>;
    return thenImplNoError<ActualOutput, Output>(js, kj::fwd<Func>(func));
  }

  // Attach an error handler only; success values pass through unchanged.
  // CONSUMES the promise - cannot call .catch_() again on the same promise.
  // The handler receives (Lock& js, kj::Exception exception) and must return T
  // (the same type as the promise) to recover from the error.
  template <typename ErrorFunc>
  DeferredPromise<T> catch_(Lock& js, ErrorFunc&& errorFunc) {
    static_assert(
        kj::isSameType<T,
            RemoveDeferredPromise<RemovePromise<ReturnType<ErrorFunc, kj::Exception, true>>>>(),
        "function passed to .catch_() must return exactly the promise's type");

    return catchImpl(js, kj::fwd<ErrorFunc>(errorFunc));
  }

  // Get a void promise that settles when this promise settles.
  // DOES NOT CONSUME the promise - you can still call .then() after this.
  // Propagates rejections: if the original promise rejects, this rejects with
  // the same exception.
  DeferredPromise<void> whenResolved(Lock& js) {
    using Pending = typename _::DeferredPromiseState<T>::Pending;
    using Resolved = typename _::DeferredPromiseState<T>::Resolved;
    using Rejected = typename _::DeferredPromiseState<T>::Rejected;
    using Consumed = typename _::DeferredPromiseState<T>::Consumed;

    KJ_SWITCH_ONEOF(state->state.underlying()) {
      KJ_CASE_ONEOF(pending, Pending) {
        // Create a new void promise that will be resolved/rejected when this one settles
        auto resultState = kj::rc<_::DeferredPromiseState<void>>();
        auto resultStateRef = resultState.addRef();

        // Add an observer that resolves/rejects the void promise
        pending.resolutionObservers.add(
            [rs = kj::mv(resultState)](Lock& js, kj::Maybe<kj::Exception> maybeException) mutable {
          KJ_IF_SOME(exception, maybeException) {
            rs->reject(js, kj::mv(exception));
          } else {
            rs->resolve(js);
          }
        });

        auto result = DeferredPromise<void>(kj::mv(resultStateRef));
        if (state->markedAsHandled) {
          result.markAsHandled(js);
        }
        return result;
      }
      KJ_CASE_ONEOF(resolved, Resolved) {
        // Already resolved - return an already-resolved void promise
        return DeferredPromise<void>::resolved();
      }
      KJ_CASE_ONEOF(rejected, Rejected) {
        // Already rejected - return an already-rejected void promise with the same exception
        return DeferredPromise<void>::rejected(js, kj::cp(rejected.exception));
      }
      KJ_CASE_ONEOF(consumed, Consumed) {
        KJ_FAIL_REQUIRE("DeferredPromise already consumed");
      }
    }
    KJ_UNREACHABLE;
  }

  // ======================================================================================
  // Conversion to jsg::Promise

  // Convert this DeferredPromise to a jsg::Promise<T>.
  // CONSUMES the promise - cannot call .then() or toJsPromise() again.
  //
  // This triggers V8 promise creation if the promise is still pending.
  // Use when you need to return a promise to JavaScript code or integrate
  // with APIs that expect jsg::Promise.
  //
  // If already resolved/rejected, returns an immediately settled jsg::Promise.
  Promise<T> toJsPromise(Lock& js) {
    using Pending = typename _::DeferredPromiseState<T>::Pending;
    using Resolved = typename _::DeferredPromiseState<T>::Resolved;
    using Rejected = typename _::DeferredPromiseState<T>::Rejected;
    using Consumed = typename _::DeferredPromiseState<T>::Consumed;

    KJ_SWITCH_ONEOF(state->state.underlying()) {
      KJ_CASE_ONEOF(pending, Pending) {
        // Ensure promise hasn't already been consumed
        KJ_REQUIRE(pending.thenCallback == kj::none,
            "DeferredPromise already consumed - cannot convert to jsg::Promise");

        // Create JS promise/resolver pair
        auto pair = js.newPromiseAndResolver<T>();
        pending.jsResolver = kj::mv(pair.resolver);
        if (state->markedAsHandled) {
          pair.promise.markAsHandled(js);
        }
        return kj::mv(pair.promise);
      }
      KJ_CASE_ONEOF(resolved, Resolved) {
        // Extract value before transition since reference becomes invalid
        if constexpr (isVoid<T>()) {
          state->state.template transitionTo<Consumed>();
          return js.resolvedPromise();
        } else {
          auto value = kj::mv(const_cast<Resolved&>(resolved).value);
          state->state.template transitionTo<Consumed>();
          return js.resolvedPromise(kj::mv(value));
        }
      }
      KJ_CASE_ONEOF(rejected, Rejected) {
        // Extract exception before transition since reference becomes invalid
        // Note: forceTransitionTo needed because ErrorState makes Rejected implicitly terminal
        auto exception = kj::mv(const_cast<Rejected&>(rejected).exception);
        state->state.template forceTransitionTo<Consumed>();
        // Convert kj::Exception to JS at the boundary
        return js.rejectedPromise<T>(js.exceptionToJs(kj::mv(exception)).getHandle(js));
      }
      KJ_CASE_ONEOF(consumed, Consumed) {
        KJ_FAIL_REQUIRE("DeferredPromise already consumed");
      }
    }
    KJ_UNREACHABLE;
  }

  // ======================================================================================
  // State Queries
  // Check the current state of the promise. These methods are useful for
  // optimization paths where you want to handle already-settled promises
  // differently from pending ones.

  // True if the promise is not yet settled (neither resolved nor rejected).
  inline bool isPending() const {
    return state->isPending();
  }

  // True if the promise was resolved with a value.
  inline bool isResolved() const {
    return state->isResolved();
  }

  // True if the promise was rejected with an error.
  inline bool isRejected() const {
    return state->isRejected();
  }

  // Optimization: Get the resolved value if already resolved, consuming the promise.
  // Returns kj::none if pending or rejected.
  // This is useful for fast-path handling when the value is expected
  // to be immediately available.
  // CONSUMES the promise - cannot call .then() or tryConsumeResolved() again.
  kj::Maybe<T> tryConsumeResolved()
    requires NonVoidType<T>
  {
    using Resolved = typename _::DeferredPromiseState<T>::Resolved;
    using Consumed = typename _::DeferredPromiseState<T>::Consumed;
    KJ_IF_SOME(resolved, state->state.template tryGetUnsafe<Resolved>()) {
      auto value = kj::mv(resolved.value);
      state->state.template transitionTo<Consumed>();
      return kj::mv(value);
    }
    return kj::none;
  }

  // Optimization: Get the rejection exception if already rejected, consuming the promise.
  // Returns kj::none if pending or resolved.
  // This is useful for fast-path error handling when the exception is expected
  // to be immediately available.
  // CONSUMES the promise - cannot call .then() or tryConsumeRejected() again.
  kj::Maybe<kj::Exception> tryConsumeRejected() {
    using Rejected = typename _::DeferredPromiseState<T>::Rejected;
    using Consumed = typename _::DeferredPromiseState<T>::Consumed;
    KJ_IF_SOME(rejected, state->state.template tryGetUnsafe<Rejected>()) {
      auto exception = kj::mv(rejected.exception);
      // Note: forceTransitionTo needed because ErrorState makes Rejected implicitly terminal
      state->state.template forceTransitionTo<Consumed>();
      return kj::mv(exception);
    }
    return kj::none;
  }

  // ======================================================================================
  // GC Integration

  // Trace JavaScript values held by this promise for garbage collection.
  void visitForGc(GcVisitor& visitor) {
    state->visitForGc(visitor);
  }

  JSG_MEMORY_INFO(DeferredPromise) {
    tracker.trackField("state", state);
  }

 private:
  template <typename U>
  friend class DeferredPromise;

  template <typename U>
  friend class DeferredPromiseResolver;

  template <typename U>
  friend DeferredPromiseResolverPair<U> newDeferredPromiseAndResolver();

  kj::Rc<_::DeferredPromiseState<T>> state;

  explicit DeferredPromise(kj::Rc<_::DeferredPromiseState<T>> s): state(kj::mv(s)) {}

  // Default constructor creates pending state - use factory methods instead
  DeferredPromise(): state(kj::rc<_::DeferredPromiseState<T>>()) {}

  // Helper to resolve the result state, handling promise chaining
  template <typename RawOutput, typename Output>
  static void resolveWithChaining(
      Lock& js, kj::Rc<_::DeferredPromiseState<Output>>& rs, RawOutput&& result) {
    if constexpr (kIsDeferredPromise<RawOutput>) {
      // Result is a DeferredPromise - chain it
      // Handle void vs non-void inner types separately to avoid "reference to void"
      // Note: DeferredPromise error handlers receive kj::Exception
      if constexpr (isVoid<Output>()) {
        result.then(js, [rs = rs.addRef()](Lock& js) mutable { rs->resolve(js); },
            [rs = rs.addRef()](
                Lock& js, kj::Exception exception) mutable { rs->reject(js, kj::mv(exception)); });
      } else {
        result.then(js, [rs = rs.addRef()](Lock& js, Output innerValue) mutable {
          rs->resolve(js, kj::mv(innerValue));
        }, [rs = rs.addRef()](Lock& js, kj::Exception exception) mutable {
          rs->reject(js, kj::mv(exception));
        });
      }
    } else if constexpr (kIsJsgPromiseType<RawOutput>) {
      // Result is a jsg::Promise - chain it via .then()
      // Note: jsg::Promise error handlers receive Value
      if constexpr (isVoid<Output>()) {
        result.then(js, [rs = rs.addRef()](Lock& js) mutable { rs->resolve(js); },
            [rs = rs.addRef()](Lock& js, Value error) mutable { rs->reject(js, kj::mv(error)); });
      } else {
        result.then(js, [rs = rs.addRef()](Lock& js, Output innerValue) mutable {
          rs->resolve(js, kj::mv(innerValue));
        }, [rs = rs.addRef()](Lock& js, Value error) mutable { rs->reject(js, kj::mv(error)); });
      }
    } else {
      // Result is a plain value - resolve directly
      if constexpr (isVoid<Output>()) {
        rs->resolve(js);
      } else {
        rs->resolve(js, kj::fwd<RawOutput>(result));
      }
    }
  }

  // thenImpl with error handler
  template <typename RawOutput, typename Output, typename Func, typename ErrorFunc>
  DeferredPromise<Output> thenImpl(Lock& js, Func&& func, ErrorFunc&& errorFunc) {
    using Pending = typename _::DeferredPromiseState<T>::Pending;
    using Resolved = typename _::DeferredPromiseState<T>::Resolved;
    using Rejected = typename _::DeferredPromiseState<T>::Rejected;
    using Consumed = typename _::DeferredPromiseState<T>::Consumed;

    // Capture the address of the code that called .then() for async stack traces.
    // This will point to user code, not DeferredPromise internals.
    void* continuationTrace = JSG_GET_RETURN_ADDRESS();

    // Capture the current async context frame to restore when continuation runs.
    auto asyncContext = AsyncContextScope::capture(js);

    static constexpr auto maybeAddRef = [](kj::Maybe<jsg::Ref<AsyncContextFrame>&> ref) {
      return ref.map([](jsg::Ref<AsyncContextFrame>& r) { return r.addRef(); });
    };

    KJ_SWITCH_ONEOF(state->state.underlying()) {
      KJ_CASE_ONEOF(pending, Pending) {
        // Ensure promise hasn't already been consumed
        KJ_REQUIRE(pending.thenCallback == kj::none,
            "DeferredPromise already consumed - .then() can only be called once");

        // Create the result promise's shared state - only needed for pending case
        auto resultState = kj::rc<_::DeferredPromiseState<Output>>();
        auto resultStateRef = resultState.addRef();

        // Set the success callback
        if constexpr (isVoid<T>()) {
          pending.thenCallback = [f = kj::mv(func), rs = kj::mv(resultState), continuationTrace,
                                     asyncContext = maybeAddRef(asyncContext)](Lock& js) mutable {
            // Enter the async context that was current when .then() was called
            AsyncContextScope asyncScope(js, asyncContext);
            try {
              if constexpr (isVoid<RawOutput>()) {
                f(js);
                rs->resolve(js);
              } else {
                auto result = f(js);
                resolveWithChaining<RawOutput, Output>(js, rs, kj::mv(result));
              }
            } catch (JsExceptionThrown&) {
              auto ex = kj::getCaughtExceptionAsKj();
              ex.addTrace(continuationTrace);
              rs->reject(js, js.exceptionToJs(kj::mv(ex)));
            } catch (...) {
              auto ex = kj::getCaughtExceptionAsKj();
              ex.addTrace(continuationTrace);
              rs->reject(js, kj::mv(ex));
            }
          };
        } else {
          pending.thenCallback = [f = kj::mv(func), rs = kj::mv(resultState), continuationTrace,
                                     asyncContext = maybeAddRef(asyncContext)](
                                     Lock& js, T value) mutable {
            // Enter the async context that was current when .then() was called
            AsyncContextScope asyncScope(js, asyncContext);
            try {
              if constexpr (isVoid<RawOutput>()) {
                f(js, kj::mv(value));
                rs->resolve(js);
              } else {
                auto result = f(js, kj::mv(value));
                resolveWithChaining<RawOutput, Output>(js, rs, kj::mv(result));
              }
            } catch (JsExceptionThrown&) {
              auto ex = kj::getCaughtExceptionAsKj();
              ex.addTrace(continuationTrace);
              rs->reject(js, js.exceptionToJs(kj::mv(ex)));
            } catch (...) {
              auto ex = kj::getCaughtExceptionAsKj();
              ex.addTrace(continuationTrace);
              rs->reject(js, kj::mv(ex));
            }
          };
        }

        // Set the error callback - receives kj::Exception directly
        // Note: asyncContext was moved into thenCallback above, so we need to capture it
        // fresh here. Both callbacks need to restore the same async context.
        pending.catchCallback = [ef = kj::mv(errorFunc), rs = resultStateRef.addRef(),
                                    continuationTrace, asyncContext = maybeAddRef(asyncContext)](
                                    Lock& js, kj::Exception exception) mutable {
          // Enter the async context that was current when .then() was called
          AsyncContextScope asyncScope(js, asyncContext);
          try {
            if constexpr (isVoid<RawOutput>()) {
              ef(js, kj::mv(exception));
              rs->resolve(js);
            } else {
              auto result = ef(js, kj::mv(exception));
              resolveWithChaining<RawOutput, Output>(js, rs, kj::mv(result));
            }
          } catch (JsExceptionThrown&) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            rs->reject(js, kj::mv(ex));
          } catch (...) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            rs->reject(js, kj::mv(ex));
          }
        };

        return DeferredPromise<Output>(kj::mv(resultStateRef));
      }
      KJ_CASE_ONEOF(resolved, Resolved) {
        // Already resolved - execute continuation immediately, mark as consumed
        // Extract value before transition since reference becomes invalid
        if constexpr (isVoid<T>()) {
          state->state.template transitionTo<Consumed>();
          try {
            if constexpr (isVoid<RawOutput>()) {
              func(js);
              return DeferredPromise<Output>::resolved();
            } else {
              auto result = func(js);
              if constexpr (kIsDeferredPromise<RawOutput>) {
                return kj::mv(result);  // Already DeferredPromise<Output>
              } else if constexpr (kIsJsgPromiseType<RawOutput>) {
                // Convert jsg::Promise to DeferredPromise by wrapping
                auto resultState = kj::rc<_::DeferredPromiseState<Output>>();
                auto resultStateRef = resultState.addRef();
                resolveWithChaining<RawOutput, Output>(js, resultState, kj::mv(result));
                return DeferredPromise<Output>(kj::mv(resultStateRef));
              } else {
                return DeferredPromise<Output>::resolved(kj::mv(result));
              }
            }
          } catch (JsExceptionThrown&) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            return DeferredPromise<Output>::rejected(js, js.exceptionToJs(kj::mv(ex)));
          } catch (...) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            return DeferredPromise<Output>::rejected(js, kj::mv(ex));
          }
        } else {
          auto value = kj::mv(const_cast<Resolved&>(resolved).value);
          state->state.template transitionTo<Consumed>();
          try {
            if constexpr (isVoid<RawOutput>()) {
              func(js, kj::mv(value));
              return DeferredPromise<Output>::resolved();
            } else {
              auto result = func(js, kj::mv(value));
              if constexpr (kIsDeferredPromise<RawOutput>) {
                return kj::mv(result);  // Already DeferredPromise<Output>
              } else if constexpr (kIsJsgPromiseType<RawOutput>) {
                // Convert jsg::Promise to DeferredPromise by wrapping
                auto resultState = kj::rc<_::DeferredPromiseState<Output>>();
                auto resultStateRef = resultState.addRef();
                resolveWithChaining<RawOutput, Output>(js, resultState, kj::mv(result));
                return DeferredPromise<Output>(kj::mv(resultStateRef));
              } else {
                return DeferredPromise<Output>::resolved(kj::mv(result));
              }
            }
          } catch (JsExceptionThrown&) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            return DeferredPromise<Output>::rejected(js, js.exceptionToJs(kj::mv(ex)));
          } catch (...) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            return DeferredPromise<Output>::rejected(js, kj::mv(ex));
          }
        }
      }
      KJ_CASE_ONEOF(rejected, Rejected) {
        // Already rejected - call error handler, mark as consumed
        // Extract exception before transition since the reference becomes invalid after
        // Note: forceTransitionTo needed because ErrorState makes Rejected implicitly terminal
        auto exception = kj::mv(const_cast<Rejected&>(rejected).exception);
        state->state.template forceTransitionTo<Consumed>();
        try {
          if constexpr (isVoid<RawOutput>()) {
            errorFunc(js, kj::mv(exception));
            return DeferredPromise<Output>::resolved();
          } else {
            auto result = errorFunc(js, kj::mv(exception));
            if constexpr (kIsDeferredPromise<RawOutput>) {
              return kj::mv(result);
            } else if constexpr (kIsJsgPromiseType<RawOutput>) {
              auto resultState = kj::rc<_::DeferredPromiseState<Output>>();
              auto resultStateRef = resultState.addRef();
              resolveWithChaining<RawOutput, Output>(js, resultState, kj::mv(result));
              return DeferredPromise<Output>(kj::mv(resultStateRef));
            } else {
              return DeferredPromise<Output>::resolved(kj::mv(result));
            }
          }
        } catch (JsExceptionThrown&) {
          auto ex = kj::getCaughtExceptionAsKj();
          ex.addTrace(continuationTrace);
          return DeferredPromise<Output>::rejected(js, kj::mv(ex));
        } catch (...) {
          auto ex = kj::getCaughtExceptionAsKj();
          ex.addTrace(continuationTrace);
          return DeferredPromise<Output>::rejected(js, kj::mv(ex));
        }
      }
      KJ_CASE_ONEOF(consumed, Consumed) {
        KJ_FAIL_REQUIRE("DeferredPromise already consumed");
      }
    }
    KJ_UNREACHABLE;
  }

  // thenImpl without error handler - propagates errors
  template <typename RawOutput, typename Output, typename Func>
  DeferredPromise<Output> thenImplNoError(Lock& js, Func&& func) {
    using Pending = typename _::DeferredPromiseState<T>::Pending;
    using Resolved = typename _::DeferredPromiseState<T>::Resolved;
    using Rejected = typename _::DeferredPromiseState<T>::Rejected;
    using Consumed = typename _::DeferredPromiseState<T>::Consumed;

    // Capture the address of the code that called .then() for async stack traces.
    // This will point to user code, not DeferredPromise internals.
    void* continuationTrace = JSG_GET_RETURN_ADDRESS();

    // Capture the current async context frame to restore when continuation runs.
    auto asyncContext = AsyncContextScope::capture(js);

    static constexpr auto maybeAddRef = [](kj::Maybe<jsg::Ref<AsyncContextFrame>&> ref) {
      return ref.map([](jsg::Ref<AsyncContextFrame>& r) { return r.addRef(); });
    };

    KJ_SWITCH_ONEOF(state->state.underlying()) {
      KJ_CASE_ONEOF(pending, Pending) {
        // Ensure promise hasn't already been consumed
        KJ_REQUIRE(pending.thenCallback == kj::none,
            "DeferredPromise already consumed - .then() can only be called once");

        // Create the result promise's shared state - only needed for pending case
        auto resultState = kj::rc<_::DeferredPromiseState<Output>>();
        auto resultStateRef = resultState.addRef();

        // Set the success callback
        if constexpr (isVoid<T>()) {
          pending.thenCallback = [f = kj::mv(func), rs = kj::mv(resultState), continuationTrace,
                                     asyncContext = maybeAddRef(asyncContext)](Lock& js) mutable {
            // Enter the async context that was current when .then() was called
            AsyncContextScope asyncScope(js, asyncContext);
            try {
              if constexpr (isVoid<RawOutput>()) {
                f(js);
                rs->resolve(js);
              } else {
                auto result = f(js);
                resolveWithChaining<RawOutput, Output>(js, rs, kj::mv(result));
              }
            } catch (JsExceptionThrown&) {
              auto ex = kj::getCaughtExceptionAsKj();
              ex.addTrace(continuationTrace);
              rs->reject(js, js.exceptionToJs(kj::mv(ex)));
            } catch (...) {
              auto ex = kj::getCaughtExceptionAsKj();
              ex.addTrace(continuationTrace);
              rs->reject(js, kj::mv(ex));
            }
          };
        } else {
          pending.thenCallback = [f = kj::mv(func), rs = kj::mv(resultState), continuationTrace,
                                     asyncContext = maybeAddRef(asyncContext)](
                                     Lock& js, T value) mutable {
            // Enter the async context that was current when .then() was called
            AsyncContextScope asyncScope(js, asyncContext);
            try {
              if constexpr (isVoid<RawOutput>()) {
                f(js, kj::mv(value));
                rs->resolve(js);
              } else {
                auto result = f(js, kj::mv(value));
                resolveWithChaining<RawOutput, Output>(js, rs, kj::mv(result));
              }
            } catch (JsExceptionThrown&) {
              auto ex = kj::getCaughtExceptionAsKj();
              ex.addTrace(continuationTrace);
              rs->reject(js, js.exceptionToJs(kj::mv(ex)));
            } catch (...) {
              auto ex = kj::getCaughtExceptionAsKj();
              ex.addTrace(continuationTrace);
              rs->reject(js, kj::mv(ex));
            }
          };
        }

        // No error handler - propagate rejection (exception passed through directly)
        // No need to restore async context since we're just propagating the exception.
        pending.catchCallback = [rs = resultStateRef.addRef()](
                                    Lock& js, kj::Exception exception) mutable {
          rs->reject(js, kj::mv(exception));
        };

        return DeferredPromise<Output>(kj::mv(resultStateRef));
      }
      KJ_CASE_ONEOF(resolved, Resolved) {
        // Already resolved - execute continuation immediately, mark as consumed
        // Extract value before transition since reference becomes invalid
        if constexpr (isVoid<T>()) {
          state->state.template transitionTo<Consumed>();
          try {
            if constexpr (isVoid<RawOutput>()) {
              func(js);
              return DeferredPromise<Output>::resolved();
            } else {
              auto result = func(js);
              if constexpr (kIsDeferredPromise<RawOutput>) {
                return kj::mv(result);  // Already DeferredPromise<Output>
              } else if constexpr (kIsJsgPromiseType<RawOutput>) {
                // Convert jsg::Promise to DeferredPromise by wrapping
                auto resultState = kj::rc<_::DeferredPromiseState<Output>>();
                auto resultStateRef = resultState.addRef();
                resolveWithChaining<RawOutput, Output>(js, resultState, kj::mv(result));
                return DeferredPromise<Output>(kj::mv(resultStateRef));
              } else {
                return DeferredPromise<Output>::resolved(kj::mv(result));
              }
            }
          } catch (JsExceptionThrown&) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            return DeferredPromise<Output>::rejected(js, js.exceptionToJs(kj::mv(ex)));
          } catch (...) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            return DeferredPromise<Output>::rejected(js, kj::mv(ex));
          }
        } else {
          auto value = kj::mv(const_cast<Resolved&>(resolved).value);
          state->state.template transitionTo<Consumed>();
          try {
            if constexpr (isVoid<RawOutput>()) {
              func(js, kj::mv(value));
              return DeferredPromise<Output>::resolved();
            } else {
              auto result = func(js, kj::mv(value));
              if constexpr (kIsDeferredPromise<RawOutput>) {
                return kj::mv(result);  // Already DeferredPromise<Output>
              } else if constexpr (kIsJsgPromiseType<RawOutput>) {
                // Convert jsg::Promise to DeferredPromise by wrapping
                auto resultState = kj::rc<_::DeferredPromiseState<Output>>();
                auto resultStateRef = resultState.addRef();
                resolveWithChaining<RawOutput, Output>(js, resultState, kj::mv(result));
                return DeferredPromise<Output>(kj::mv(resultStateRef));
              } else {
                return DeferredPromise<Output>::resolved(kj::mv(result));
              }
            }
          } catch (JsExceptionThrown&) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            return DeferredPromise<Output>::rejected(js, js.exceptionToJs(kj::mv(ex)));
          } catch (...) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            return DeferredPromise<Output>::rejected(js, kj::mv(ex));
          }
        }
      }
      KJ_CASE_ONEOF(rejected, Rejected) {
        // Already rejected - propagate, mark as consumed
        // Extract exception before transition since reference becomes invalid
        // Note: forceTransitionTo needed because ErrorState makes Rejected implicitly terminal
        auto exception = kj::mv(const_cast<Rejected&>(rejected).exception);
        state->state.template forceTransitionTo<Consumed>();
        return DeferredPromise<Output>::rejected(js, kj::mv(exception));
      }
      KJ_CASE_ONEOF(consumed, Consumed) {
        KJ_FAIL_REQUIRE("DeferredPromise already consumed");
      }
    }
    KJ_UNREACHABLE;
  }

  template <typename ErrorFunc>
  DeferredPromise<T> catchImpl(Lock& js, ErrorFunc&& errorFunc) {
    using Pending = typename _::DeferredPromiseState<T>::Pending;
    using Resolved = typename _::DeferredPromiseState<T>::Resolved;
    using Rejected = typename _::DeferredPromiseState<T>::Rejected;
    using Consumed = typename _::DeferredPromiseState<T>::Consumed;

    // Capture the address of the code that called .catch_() for async stack traces.
    // This will point to user code, not DeferredPromise internals.
    void* continuationTrace = JSG_GET_RETURN_ADDRESS();

    // Capture the current async context frame to restore when error handler runs.
    auto asyncContext = AsyncContextScope::capture(js);

    static constexpr auto maybeAddRef = [](kj::Maybe<jsg::Ref<AsyncContextFrame>&> ref) {
      return ref.map([](jsg::Ref<AsyncContextFrame>& r) { return r.addRef(); });
    };

    KJ_SWITCH_ONEOF(state->state.underlying()) {
      KJ_CASE_ONEOF(pending, Pending) {
        // Ensure promise hasn't already been consumed
        KJ_REQUIRE(pending.thenCallback == kj::none,
            "DeferredPromise already consumed - .catch_() can only be called once");

        // Create the result promise's shared state - only needed for pending case
        auto resultState = kj::rc<_::DeferredPromiseState<T>>();
        auto resultStateRef = resultState.addRef();

        // Success just propagates - no user callback invoked, no async context needed
        if constexpr (isVoid<T>()) {
          pending.thenCallback = [rs = kj::mv(resultState)](Lock& js) mutable { rs->resolve(js); };
        } else {
          pending.thenCallback = [rs = kj::mv(resultState)](
                                     Lock& js, T value) mutable { rs->resolve(js, kj::mv(value)); };
        }

        // Error calls the handler - receives kj::Exception directly
        pending.catchCallback = [ef = kj::mv(errorFunc), rs = resultStateRef.addRef(),
                                    continuationTrace, asyncContext = maybeAddRef(asyncContext)](
                                    Lock& js, kj::Exception exception) mutable {
          // Enter the async context that was current when .catch_() was called
          AsyncContextScope asyncScope(js, asyncContext);
          try {
            if constexpr (isVoid<T>()) {
              ef(js, kj::mv(exception));
              rs->resolve(js);
            } else {
              rs->resolve(js, ef(js, kj::mv(exception)));
            }
          } catch (JsExceptionThrown&) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            rs->reject(js, kj::mv(ex));
          } catch (...) {
            auto ex = kj::getCaughtExceptionAsKj();
            ex.addTrace(continuationTrace);
            rs->reject(js, kj::mv(ex));
          }
        };

        return DeferredPromise<T>(kj::mv(resultStateRef));
      }
      KJ_CASE_ONEOF(resolved, Resolved) {
        // Already resolved - just propagate, mark as consumed
        // Extract value before transition since reference becomes invalid
        if constexpr (isVoid<T>()) {
          state->state.template transitionTo<Consumed>();
          return DeferredPromise<void>::resolved();
        } else {
          auto value = kj::mv(const_cast<Resolved&>(resolved).value);
          state->state.template transitionTo<Consumed>();
          return DeferredPromise<T>::resolved(kj::mv(value));
        }
      }
      KJ_CASE_ONEOF(rejected, Rejected) {
        // Already rejected - call handler, mark as consumed
        // Extract exception before transition since reference becomes invalid
        // Note: forceTransitionTo needed because ErrorState makes Rejected implicitly terminal
        auto exception = kj::mv(const_cast<Rejected&>(rejected).exception);
        state->state.template forceTransitionTo<Consumed>();
        try {
          if constexpr (isVoid<T>()) {
            errorFunc(js, kj::mv(exception));
            return DeferredPromise<void>::resolved();
          } else {
            return DeferredPromise<T>::resolved(errorFunc(js, kj::mv(exception)));
          }
        } catch (JsExceptionThrown&) {
          auto ex = kj::getCaughtExceptionAsKj();
          ex.addTrace(continuationTrace);
          return DeferredPromise<T>::rejected(js, kj::mv(ex));
        } catch (...) {
          auto ex = kj::getCaughtExceptionAsKj();
          ex.addTrace(continuationTrace);
          return DeferredPromise<T>::rejected(js, kj::mv(ex));
        }
      }
      KJ_CASE_ONEOF(consumed, Consumed) {
        KJ_FAIL_REQUIRE("DeferredPromise already consumed");
      }
    }
    KJ_UNREACHABLE;
  }
};

// ======================================================================================
// Factory Functions
//
// Primary way to create DeferredPromise instances. Creates a promise/resolver
// pair - pass the promise to consumers and keep the resolver to control when
// the promise resolves.
//
// Usage:
//   auto pair = newDeferredPromiseAndResolver<int>();
//   someAsyncApi(kj::mv(pair.promise));  // Consumer attaches .then()
//   // ... later ...
//   pair.resolver.resolve(js, 42);  // Triggers all continuations
//
// Or via jsg::Lock for convenience:
//   auto pair = js.newDeferredPromiseAndResolver<int>();

// The result type returned by newDeferredPromiseAndResolver().
template <typename T>
struct DeferredPromiseResolverPair {
  DeferredPromise<T> promise;
  DeferredPromiseResolver<T> resolver;
};

// Create a new pending DeferredPromise and its associated Resolver.
// The promise and resolver share ownership of the underlying state.
template <typename T>
inline DeferredPromiseResolverPair<T> newDeferredPromiseAndResolver() {
  auto state = kj::rc<_::DeferredPromiseState<T>>();
  auto stateRef = state.addRef();
  return {.promise = DeferredPromise<T>(kj::mv(state)),
    .resolver = DeferredPromiseResolver<T>(kj::mv(stateRef))};
}

// A key difference between jsg::Promise and jsg::DeferredPromise is that the
// latter does not preserve the reference to the original JS Promise object and
// will not roundtrip to produce the same promise.
template <typename TypeWrapper>
class DeferredPromiseWrapper {
 public:
  template <typename T>
  static constexpr const char* getName(DeferredPromise<T>*) {
    return "Promise";
  }

  template <typename T>
  v8::Local<v8::Promise> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      DeferredPromise<T>&& promise) {
    KJ_IF_SOME(ex, promise.tryConsumeRejected()) {
      // The promise is already rejected, create an immediately rejected JS promise
      // to avoid the overhead of creating a full jsg::Promise.
      auto jsError = js.exceptionToJsValue(kj::mv(ex));
      auto v8PromiseResolver = check(v8::Promise::Resolver::New(context));
      check(v8PromiseResolver->Reject(context, jsError.getHandle(js)));
      return v8PromiseResolver->GetPromise();
    }

    auto& wrapper = *static_cast<TypeWrapper*>(this);
    KJ_IF_SOME(value, promise.tryConsumeResolved()) {
      // The promise is already resolved, create an immediately resolved JS promise
      // to avoid the overhead of creating a full jsg::Promise and an additional microtask.
      auto v8PromiseResolver = check(v8::Promise::Resolver::New(context));
      if constexpr (isVoid<T>()) {
        check(v8PromiseResolver->Resolve(context, v8::Undefined(js.v8Isolate)));
      } else {
        auto jsValue = wrapper.wrap(js, context, creator, kj::mv(value));
        check(v8PromiseResolver->Resolve(context, jsValue));
      }
      return v8PromiseResolver->GetPromise();
    }

    // The deferred promise is still pending, wrap it as a jsg::Promise to handle
    // continuations and eventual unwrapping of the result.
    return wrapper.wrap(js, context, creator, promise.toJsPromise(js));
  }

  template <typename T>
  kj::Maybe<DeferredPromise<T>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      DeferredPromise<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto& wrapper = *static_cast<TypeWrapper*>(this);

    // If the handle is a Promise that is already resolved or rejected, we can optimize
    // by creating a DeferredPromise that is already settled rather than going through
    // the full jsg::Promise unwrapping process.
    if (handle->IsPromise()) {
      auto promise = handle.As<v8::Promise>();
      switch (promise->State()) {
        case v8::Promise::PromiseState::kPending: {
          // The promise is still pending, fall through to normal unwrapping via jsg::Promise.
          break;
        }
        case v8::Promise::PromiseState::kFulfilled: {
          // The promise is already fulfilled, create an already-resolved DeferredPromise.
          if constexpr (isVoid<T>()) {
            return DeferredPromise<T>::resolved();
          } else {
            KJ_IF_SOME(value,
                wrapper.tryUnwrap(
                    js, context, promise->Result(), static_cast<T*>(nullptr), parentObject)) {
              return DeferredPromise<T>::resolved(kj::mv(value));
            }
            return kj::none;
          }
        }
        case v8::Promise::PromiseState::kRejected: {
          // The promise is already rejected, create an already-rejected DeferredPromise.
          auto exception = js.exceptionToKj(js.v8Ref(promise->Result()));
          return DeferredPromise<T>::rejected(js, kj::mv(exception));
        }
      }

      // Promise is still pending, Unwrap via jsg::Promise.
      KJ_IF_SOME(jsPromise,
          wrapper.tryUnwrap(js, context, handle, static_cast<Promise<T>*>(nullptr), parentObject)) {
        return DeferredPromise<T>::fromJsPromise(js, kj::mv(jsPromise));
      }
      return kj::none;
    } else {
      // Value is not a Promise. Treat it as an already-resolved value.

      // If the value is thenable, we need to convert it into a proper Promise first.
      // Unfortunately there's no optimized way to do this, we have to pass it through
      // a Promise microtask.
      if (isThenable(context, handle)) {
        auto paf = check(v8::Promise::Resolver::New(context));
        check(paf->Resolve(context, handle));
        return tryUnwrap(js, context, paf->GetPromise(), static_cast<DeferredPromise<T>*>(nullptr),
            parentObject);
      }

      // The value is not thenable, treat it as a resolved value.
      if constexpr (isVoid<T>()) {
        return DeferredPromise<T>::resolved();
      } else {
        KJ_IF_SOME(value, wrapper.tryUnwrap(js, context, handle, (T*)nullptr, parentObject)) {
          return DeferredPromise<T>::resolved(kj::mv(value));
        }
        return kj::none;
      }
    }
  }

 private:
  static bool isThenable(v8::Local<v8::Context> context, v8::Local<v8::Value> handle) {
    if (handle->IsObject()) {
      auto obj = handle.As<v8::Object>();
      return check(obj->Has(context, v8StrIntern(v8::Isolate::GetCurrent(), "then")));
    }
    return false;
  }
};

// ======================================================================================
// When NOT To Use DeferredPromise (Even For Pure C++ Code)
// ======================================================================================
//
// DeferredPromise executes continuations SYNCHRONOUSLY when resolve() is called.
// This is the source of its performance benefits, but it also creates semantic
// differences from jsg::Promise that can cause bugs even when the promise never
// crosses into JavaScript. Consider these scenarios carefully:
//
// -----------------------------------------------------------------------------
// 1. REENTRANCY HAZARDS
// -----------------------------------------------------------------------------
//
// With jsg::Promise, callbacks run on the microtask queue AFTER resolve() returns.
// With DeferredPromise, callbacks run DURING resolve(), before it returns.
//
// DANGEROUS PATTERN:
//
//   class DataProcessor {
//     kj::Vector<Item> pendingItems;
//     DeferredPromiseResolver<void> resolver;
//
//     void addItem(Item item) {
//       pendingItems.add(kj::mv(item));
//       if (pendingItems.size() >= BATCH_SIZE) {
//         processBatch();
//       }
//     }
//
//     void processBatch() {
//       // Process items...
//       resolver.resolve(js);  // <-- DANGER: callback might call addItem()!
//       // pendingItems may have been modified by callback reentrancy
//     }
//   };
//
// The callback attached to the promise might call back into addItem(), modifying
// pendingItems while processBatch() is still iterating or making assumptions
// about its state. With jsg::Promise, the callback would run later.
//
// SAFER ALTERNATIVE: Use jsg::Promise when callbacks might reenter your code,
// or explicitly defer resolution:
//
//   void processBatch() {
//     auto items = kj::mv(pendingItems);  // Take ownership before resolve
//     pendingItems = {};                   // Reset to known state
//     // Process items...
//     resolver.resolve(js);  // Now safe - state is consistent
//   }
//
// -----------------------------------------------------------------------------
// 2. STACK DEPTH / RECURSION LIMITS (SOLVED VIA TRAMPOLINING)
// -----------------------------------------------------------------------------
//
// NOTE: This issue has been SOLVED by the trampolining implementation.
// DeferredPromise uses a continuation queue that flattens the call stack,
// so deep chains of .then() callbacks are safe from stack overflow.
//
// The trampoline works by:
//   1. When resolve() is called, continuations are pushed onto a queue
//   2. Only the outermost resolve() drains the queue in a loop
//   3. This keeps stack depth O(1) regardless of chain length
//
// SAFE PATTERN (now works correctly):
//
//   DeferredPromise<int> processRecursively(Lock& js, int depth) {
//     if (depth == 0) return DeferredPromise<int>::resolved(0);
//     return processRecursively(js, depth - 1)
//         .then(js, [](Lock&, int v) { return v + 1; });
//   }
//   // With depth=10000, this now works without stack overflow!
//
// The trampolining maintains synchronous execution semantics (all callbacks
// complete before the outermost resolve() returns) while avoiding the stack
// buildup that direct nested calls would cause.
//
// -----------------------------------------------------------------------------
// 3. LOCK ORDERING AND DEADLOCKS
// -----------------------------------------------------------------------------
//
// If you hold a lock when calling resolve(), and a callback tries to acquire
// another lock, you may create lock ordering issues or deadlocks.
//
// DANGEROUS PATTERN:
//
//   kj::MutexGuarded<State> state;
//
//   void complete(Lock& js) {
//     auto locked = state.lockExclusive();
//     locked->finished = true;
//     resolver.resolve(js);  // <-- Callback runs while holding state lock!
//     // If callback tries to acquire another lock that someone else holds
//     // while waiting for state lock, deadlock!
//   }
//
// With jsg::Promise, the callback runs after complete() returns and releases
// the lock.
//
// SAFER ALTERNATIVE: Release locks before resolving, or use jsg::Promise.
//
// -----------------------------------------------------------------------------
// 4. EXCEPTION PROPAGATION TIMING
// -----------------------------------------------------------------------------
//
// Exceptions thrown in DeferredPromise callbacks propagate IMMEDIATELY up the
// call stack through resolve(). They are caught and converted to rejections,
// but this happens synchronously.
//
// SUBTLE DIFFERENCE:
//
//   void doWork(Lock& js) {
//     resolver.resolve(js, 42);
//     // With DeferredPromise: if callback threw, we already caught it and
//     // the downstream promise is rejected. No exception escapes here.
//     //
//     // With jsg::Promise: callback hasn't run yet! It will run later,
//     // and any exception becomes a rejection at that point.
//
//     doMoreWork();  // <-- With DeferredPromise, this runs AFTER callbacks
//                    //     With jsg::Promise, this runs BEFORE callbacks
//   }
//
// This ordering difference can matter for logging, cleanup, or state changes.
//
// -----------------------------------------------------------------------------
// 5. INTERLEAVING WITH OTHER ASYNC OPERATIONS
// -----------------------------------------------------------------------------
//
// Code that depends on microtask interleaving will behave differently.
//
// DANGEROUS PATTERN:
//
//   void setupTwoOperations(Lock& js) {
//     auto [p1, r1] = newDeferredPromiseAndResolver<int>();
//     auto [p2, r2] = newDeferredPromiseAndResolver<int>();
//
//     p1.then(js, [&](Lock& js, int v) {
//       // With jsg::Promise, p2's callback would also be queued,
//       // and they'd interleave fairly on the microtask queue.
//       // With DeferredPromise, this runs to completion first.
//       doExpensiveWork();
//     });
//
//     p2.then(js, [&](Lock& js, int v) {
//       // This callback is starved until p1's callback completes
//     });
//
//     r1.resolve(js, 1);
//     r2.resolve(js, 2);
//   }
//
// If fairness between multiple promise chains matters, jsg::Promise's
// microtask scheduling provides it automatically.
//
// -----------------------------------------------------------------------------
// 6. OBJECT LIFETIME DURING CALLBACKS
// -----------------------------------------------------------------------------
//
// When resolve() triggers callbacks synchronously, the resolver and any
// related objects are still "in use" on the stack.
//
// DANGEROUS PATTERN:
//
//   struct Operation : kj::Refcounted {
//     DeferredPromiseResolver<int> resolver;
//
//     void complete(Lock& js) {
//       resolver.resolve(js, 42);
//       // If callback drops the last reference to this Operation,
//       // we're now executing in a destroyed object!
//       this->cleanup();  // <-- Use-after-free!
//     }
//   };
//
// SAFER ALTERNATIVE: Prevent premature destruction:
//
//   void complete(Lock& js) {
//     auto self = kj::addRef(*this);  // prevent destruction during callback
//     resolver.resolve(js, 42);
//     this->cleanup();  // safe now
//   }
//
// -----------------------------------------------------------------------------
// 7. TESTING / SPECIFICATION COMPLIANCE
// -----------------------------------------------------------------------------
//
// If your code is implementing JavaScript-visible behavior or needs to match
// JavaScript Promise semantics for testing purposes, DeferredPromise's
// synchronous execution will not match the expected behavior.
//
// JavaScript promises ALWAYS run callbacks asynchronously, even for
// already-resolved promises:
//
//   // JavaScript
//   Promise.resolve(42).then(x => console.log(x));
//   console.log("after");
//   // Output: "after", then "42"
//
//   // DeferredPromise equivalent
//   DeferredPromise<int>::resolved(42).then(js, [](Lock&, int x) {
//     KJ_LOG(INFO, x);
//   });
//   KJ_LOG(INFO, "after");
//   // Output: "42", then "after"  <-- Different order!
//
// Use jsg::Promise when JavaScript-compatible ordering is required.
//
// -----------------------------------------------------------------------------
// SUMMARY: When to prefer jsg::Promise over DeferredPromise
// -----------------------------------------------------------------------------
//
// Use jsg::Promise when:
//   - Callbacks might reenter your code and modify shared state
//   - You hold locks when resolving (deadlock risk)
//   - You need fairness between multiple concurrent promise chains
//   - Object lifetime is tied to callback completion
//   - You're implementing JavaScript-visible behavior
//   - The promise will be returned to JavaScript anyway
//
// Use DeferredPromise when:
//   - Performance is critical and the above concerns don't apply
//   - Promises frequently resolve synchronously (streams, caches)
//   - You want deterministic, predictable callback timing
//   - You're building internal machinery that never exposes promises to JS
//   - You've carefully analyzed reentrancy and lifetime issues
//
// NOTE: Stack overflow from deep chains is NOT a concern - DeferredPromise
// uses trampolining to keep stack depth O(1) regardless of chain length.
//
// ======================================================================================

}  // namespace workerd::jsg
