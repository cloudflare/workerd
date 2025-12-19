# Code Patterns & Efficiency Analysis

This document analyzes coding patterns, inefficiencies, and duplicated logic in the workerd streams implementation. Focus is on actionable improvements that go beyond micro-optimizations that the compiler handles.

**Generated:** December 2025

---

## Table of Contents

1. [Duplicated Logic](#1-duplicated-logic)
2. [Inefficient Patterns](#2-inefficient-patterns)
3. [Algorithm Improvements](#3-algorithm-improvements)
4. [Code Smells](#4-code-smells)
5. [Memory Allocation Patterns](#5-memory-allocation-patterns)
6. [Error Handling Patterns](#6-error-handling-patterns)
7. [Positive Patterns Worth Preserving](#7-positive-patterns-worth-preserving)
8. [Recommendations Summary](#8-recommendations-summary)

---

## 1. Duplicated Logic

### 1.1 Reader/Writer Release Logic Duplication

**Location:** `internal.c++:854-877` and `standard.c++:238-273`

Both implementations have nearly identical release logic:

```cpp
// internal.c++
void ReadableStreamInternalController::releaseReader(
    Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) {
  KJ_IF_SOME(locked, readState.tryGet<ReaderLocked>()) {
    KJ_ASSERT(&locked.getReader() == &reader);
    KJ_IF_SOME(js, maybeJs) {
      // ... reject promise, check canceler ...
      maybeRejectPromise<void>(js, locked.getClosedFulfiller(),
          js.v8TypeError("This ReadableStream reader has been released."_kj));
    }
    locked.clear();
    if (maybeJs != kj::none) {
      readState.template init<Unlocked>();
    }
  }
}

// standard.c++ (ReadableLockImpl::releaseReader)
// Nearly identical structure
```

**Suggested Fix:** Extract common release logic into a shared utility or base class method.

---

### 1.2 State Switch Pattern Duplication

**Location:** Multiple files

The same pattern appears 50+ times across the codebase:

```cpp
KJ_SWITCH_ONEOF(state) {
  KJ_CASE_ONEOF(closed, StreamStates::Closed) {
    // handle closed
  }
  KJ_CASE_ONEOF(errored, StreamStates::Errored) {
    // handle errored
  }
  KJ_CASE_ONEOF(active, ActiveState) {
    // handle active
  }
}
```

**Suggested Fix:** Create a visitor helper:

```cpp
template<typename Closed, typename Errored, typename Active>
auto visitStreamState(auto& state, Closed&& onClosed, Errored&& onErrored, Active&& onActive) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(c, StreamStates::Closed) { return onClosed(c); }
    KJ_CASE_ONEOF(e, StreamStates::Errored) { return onErrored(e); }
    KJ_CASE_ONEOF(a, auto) { return onActive(a); }
  }
}
```

---

---

## 2. Inefficient Patterns

### 2.1 AllReader Parts Assembly

**Location:** `internal.c++:124-221`

```cpp
template <typename T>
kj::Promise<kj::Array<T>> read(ReadOption option = ReadOption::NONE) {
  kj::Vector<kj::Array<T>> parts;  // Multiple allocations
  // ... reads into parts ...

  // Final copy of all parts
  auto out = kj::heapArray<T>(runningTotal);
  copyInto<T>(out, parts.asPtr());  // Copies everything again
  co_return kj::mv(out);
}
```

**Issues:**
1. Each read creates a separate heap allocation
2. Final assembly requires copying all data again
3. When stream length is known, could allocate once upfront

**Current Optimization:** Single-part optimization exists (line 214-216)

**Additional Optimization:**
```cpp
// When length is known and fits in MAX_BUFFER_CHUNK
KJ_IF_SOME(length, maybeLength) {
  if (length <= MAX_BUFFER_CHUNK && length < limit) {
    // Single allocation path
    auto buffer = kj::heapArray<T>(length);
    auto amount = co_await input.tryRead(buffer.begin(), length, length);
    if (amount == length) {
      co_return kj::mv(buffer);  // No copy needed
    }
    // Fall through to multi-part path only if stream lied about length
  }
}
```

**Impact:** Eliminates copy for small streams with known size (~90% of cases).

---

### 2.2 Repeated Error String Construction

**Location:** Multiple files

```cpp
// Same string constructed multiple times:
return js.rejectedPromise<void>(js.v8TypeError("This WritableStream has been closed."_kj));
// ... appears 5+ times ...

return js.rejectedPromise<void>(
    js.v8TypeError("This ReadableStream only supports a single pending read request..."_kj));
// ... appears 3+ times ...
```

**Suggested Fix:** Define error message constants:

```cpp
namespace StreamErrors {
  constexpr auto STREAM_CLOSED = "This WritableStream has been closed."_kj;
  constexpr auto SINGLE_PENDING_READ =
      "This ReadableStream only supports a single pending read request at a time."_kj;
  constexpr auto READER_RELEASED = "This ReadableStream reader has been released."_kj;
  // ...
}
```

**Impact:** Reduces binary size, improves cache locality for error paths.

---

### 2.3 Unnecessary Promise Wrapper Allocation

**Location:** `standard.c++:500-540`

```cpp
jsg::Promise<void> maybeRunAlgorithm(
    jsg::Lock& js, auto& maybeAlgorithm, auto&& onSuccess, auto&& onFailure, auto&&... args) {
  KJ_IF_SOME(algorithm, maybeAlgorithm) {
    return js.tryCatch([&] {
      if (IoContext::hasCurrent()) {
        auto& ioContext = IoContext::current();
        return js
            .tryCatch([&] { return algorithm(js, kj::fwd<decltype(args)>(args)...); },
                [&](jsg::Value&& exception) { return js.rejectedPromise<void>(kj::mv(exception)); })
            .then(js, ioContext.addFunctor(kj::mv(onSuccess)),
                ioContext.addFunctor(kj::mv(onFailure)));
      } else {
        // Duplicated logic without ioContext
      }
    }, [&](jsg::Value&& exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
  }
  // If no algorithm, immediately call onSuccess
  onSuccess(js);
  return js.resolvedPromise();
}
```

**Issues:**
1. Two levels of `tryCatch` when one could suffice
2. Duplicated IoContext check
3. Creates promise even when algorithm succeeds synchronously

**Impact:** High - this is in the hot path for every stream operation.

---

### 2.4 Queue Entry Clone Overhead

**Location:** `queue.c++:51-57`

```cpp
kj::Rc<ValueQueue::Entry> ValueQueue::Entry::clone(jsg::Lock& js) {
  return addRefToThis();  // Just adds reference, efficient
}

ValueQueue::QueueEntry ValueQueue::QueueEntry::clone(jsg::Lock& js) {
  return QueueEntry{.entry = entry->clone(js)};  // Creates new QueueEntry wrapper
}
```

**Issue:** `QueueEntry::clone` creates a new struct wrapper even though Entry::clone just adds a reference.

**Note:** This is likely intentional for ownership semantics, but worth documenting.

---

## 3. Algorithm Improvements

### 3.1 Linear copyInto Could Use Single memcpy

**Location:** `internal.c++:223-231`

```cpp
template <typename T>
void copyInto(kj::ArrayPtr<T> out, kj::ArrayPtr<kj::Array<T>> in) {
  size_t pos = 0;
  for (auto& part: in) {
    KJ_DASSERT(part.size() <= out.size() - pos);
    memcpy(out.begin() + pos, part.begin(), part.size());
    pos += part.size();
  }
}
```

**Current:** O(n) memcpy calls where n = number of parts
**Issue:** For many small parts, this has poor cache behavior

**Alternative for contiguous data:** If parts are guaranteed contiguous (they're not currently), a single memcpy would be faster.

**Note:** Current implementation is correct; this is more of an observation than a bug.

---

### 3.2 Queue Consumer Linear Search

**Location:** `queue.h` (implied by ConsumerImpl design)

The queue maintains a list of consumers that must all be notified on push/close/error. This is O(consumers) for each operation.

**Current Design:**
```cpp
std::list<Consumer*> consumers;  // Linear iteration on each push
```

**Observation:** For typical use (1-2 consumers from tee), this is fine. Would only matter for pathological cases.

---

### 3.3 WriteQueue Uses std::list

**Location:** `internal.c++:155`

```cpp
std::list<WriteEvent> queue;
```

**Current:** `std::list` has poor cache locality, allocates each node separately.

**Alternative:** `std::deque` or `kj::Vector` with manual indexing could improve cache performance for typical workloads (few queued writes).

**Caveat:** `std::list` allows stable iterators during modification, which may be required.

---

### 3.4 Consumer ReadRequests Uses std::list (queue.h)

**Location:** `queue.h:552`

```cpp
struct Ready {
  workerd::RingBuffer<kj::OneOf<QueueEntry, Close>, 16> buffer;  // Good!
  std::list<ReadRequest> readRequests;  // Could be improved
  size_t queueTotalSize = 0;
};
```

**Observation:** The buffer uses `RingBuffer` (good choice for FIFO), but `readRequests` uses `std::list`.

**Note:** For typical workloads with 0-2 pending reads, `std::list` overhead may not matter, but a small inline vector or RingBuffer could be more cache-friendly.

---

## 4. Code Smells

### 4.1 Long Functions

| Function | File | Lines | Recommendation |
|----------|------|-------|----------------|
| `writeLoop` | internal.c++ | ~200 | Split by event type |
| `read` | internal.c++:523-665 | ~140 | Extract BYOB handling |
| `tryPipeFrom` | internal.c++ | ~180 | Extract pipe setup |
| `deferControllerStateChange` | standard.c++ | ~40 | Could be simpler |

---

### 4.2 Deep Nesting

**Location:** `internal.c++:623-661`

```cpp
return ioContext.awaitIoLegacy(js, kj::mv(promise))
    .then(js,
        ioContext.addFunctor([this, store = js.v8Ref(store), byteOffset, byteLength,
                                 isByob = maybeByobOptions != kj::none](jsg::Lock& js,
                                 size_t amount) mutable -> jsg::Promise<ReadResult> {
      readPending = false;
      KJ_ASSERT(amount <= byteLength);
      if (amount == 0) {
        if (!state.is<StreamStates::Errored>()) {
          doClose(js);
        }
        KJ_IF_SOME(o, owner) {
          o.signalEof(js);
        }
        if (isByob && FeatureFlags::get(js).getInternalStreamByobReturn()) {
          // ... more nesting ...
        }
        return js.resolvedPromise(ReadResult{.done = true});
      }
      // ... more code ...
    }),
    // error handler...
);
```

**Issue:** 5+ levels of nesting makes the code hard to follow.

**Suggested Fix:** Extract named helper functions:
```cpp
ReadResult createReadResult(jsg::Lock& js, v8::Local<v8::ArrayBuffer> store,
                            size_t byteOffset, size_t amount, bool isByob);
```

---

### 4.3 Boolean Parameter Proliferation

**Location:** Multiple functions

```cpp
jsg::Promise<void> closeImpl(jsg::Lock& js, bool markAsHandled);
jsg::Promise<void> close(jsg::Lock& js, bool markAsHandled = false);
jsg::Promise<void> flush(jsg::Lock& js, bool markAsHandled = false);
kj::Maybe<kj::Own<ReadableStreamSource>> removeSource(jsg::Lock& js, bool ignoreDisturbed);
```

**Issue:** Boolean parameters make call sites unclear:
```cpp
closeImpl(js, true);   // What does 'true' mean?
removeSource(js, false);  // What does 'false' mean?
```

**Suggested Fix:** Use enum or named struct:
```cpp
enum class HandleOption { Mark, DontMark };
closeImpl(js, HandleOption::Mark);

struct RemoveSourceOptions { bool ignoreDisturbed = false; };
removeSource(js, {.ignoreDisturbed = true});
```

---

### 4.4 Feature Flag Checks in Hot Paths

**Location:** `internal.c++:568, 637, 1135`

```cpp
if (FeatureFlags::get(js).getInternalStreamByobReturn()) {
  // new behavior
} else {
  // old behavior
}
```

**Issue:** Feature flag lookup in every read/write operation.

**Observation:** `FeatureFlags::get(js)` likely caches, so this may not be a real performance issue. However, if not cached, consider storing flag value in controller at construction time.

---

### 4.5 Mixed Error Creation Patterns

**Location:** Throughout codebase

```cpp
// Pattern 1: JSG_FAIL_REQUIRE
JSG_FAIL_REQUIRE(TypeError, "message");

// Pattern 2: Direct v8 error
return js.rejectedPromise<void>(js.v8TypeError("message"_kj));

// Pattern 3: kj exception
kj::throwFatalException(kj::Exception(...));

// Pattern 4: Custom helper
throwTypeErrorAndConsoleWarn("message");
```

**Issue:** Four different ways to throw errors makes code inconsistent and harder to maintain.

**Suggested Fix:** Standardize on one or two patterns with clear guidelines for when to use each.

---

## 5. Memory Allocation Patterns

### 5.1 Temporary Allocations in Write Path

**Location:** `internal.c++:922-967`

```cpp
auto chunk = KJ_ASSERT_NONNULL(value);
std::shared_ptr<v8::BackingStore> store;
// ...
auto ptr = kj::ArrayPtr<kj::byte>(static_cast<kj::byte*>(store->Data()) + byteOffset, byteLength);
queue.push_back(WriteEvent{
    .outputLock = IoContext::current().waitForOutputLocksIfNecessaryIoOwn(),
    .event = Write{
      .promise = kj::mv(prp.resolver),
      .totalBytes = store->ByteLength(),
      .ownBytes = js.v8Ref(v8::ArrayBuffer::New(js.v8Isolate, kj::mv(store))),
      .bytes = ptr,
    }});
```

**Observation:** Creates a new ArrayBuffer reference for each write. This is necessary for GC safety but could potentially be optimized for the case where the write completes synchronously.

---

### 5.2 Promise Resolver Pairs

**Location:** Everywhere

```cpp
auto prp = js.newPromiseAndResolver<void>();
prp.promise.markAsHandled(js);
// ... use prp.resolver ...
```

**Pattern:** Every async operation allocates a promise/resolver pair.

**Note:** This is fundamental to the design and cannot easily be changed, but worth noting as a source of allocation pressure.

---

## 6. Error Handling Patterns

### 6.1 Redundant State Checks

**Location:** `internal.c++:900-920`

```cpp
jsg::Promise<void> WritableStreamInternalController::write(...) {
  // Check 1
  if (isClosedOrClosing()) {
    return js.rejectedPromise<void>(js.v8TypeError("This WritableStream has been closed."_kj));
  }
  // Check 2 (redundant with state switch)
  if (isPiping()) {
    return js.rejectedPromise<void>(...);
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // Already handled by isClosedOrClosing()!
      KJ_UNREACHABLE;
    }
    // ...
  }
}
```

**Issue:** The switch case for Closed is unreachable because isClosedOrClosing() already handles it.

**Impact:** Minor - unreachable code, but adds to cognitive load.

---

### 6.2 Inconsistent Exception Translation

**Location:** `internal.c++:234-244`

```cpp
kj::Exception reasonToException(jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason,
    kj::String defaultDescription = kj::str(JSG_EXCEPTION(Error) ": Stream was cancelled.")) {
  KJ_IF_SOME(reason, maybeReason) {
    return js.exceptionToKj(js.v8Ref(reason));  // Converts JS exception
  } else {
    return kj::Exception(
        kj::Exception::Type::FAILED, __FILE__, __LINE__, kj::mv(defaultDescription));
  }
}
```

**Issue:** When reason is provided, the exception type comes from the JS exception. When not provided, it's always FAILED. This asymmetry could cause different behavior.

---

## 7. Positive Patterns Worth Preserving

### 7.1 RAII Backpressure Scope (queue.h)

**Location:** `queue.h:332-339`

```cpp
struct UpdateBackpressureScope final {
  QueueImpl& queue;
  UpdateBackpressureScope(QueueImpl& queue): queue(queue) {};
  ~UpdateBackpressureScope() noexcept(false) {
    queue.maybeUpdateBackpressure();
  }
  KJ_DISALLOW_COPY_AND_MOVE(UpdateBackpressureScope);
};
```

**Why it's good:** Ensures backpressure is always updated when buffer changes, even on exception paths. This pattern should be used consistently.

---

### 7.2 SmallSet for Consumers (queue.h)

**Location:** `queue.h:293`

```cpp
SmallSet<ConsumerImpl*> consumers;
```

**Why it's good:** Optimizes for the common case (1-2 consumers from tee) while still supporting arbitrary numbers. Avoids heap allocation for small sets.

---

### 7.3 RingBuffer for Buffer (queue.h)

**Location:** `queue.h:551`

```cpp
workerd::RingBuffer<kj::OneOf<QueueEntry, Close>, 16> buffer;
```

**Why it's good:** Fixed initial capacity of 16 avoids allocations for typical cases. Ring buffer is ideal for FIFO queues with frequent push/pop.

---

### 7.4 Intentional GC Non-Visiting (queue.h)

**Location:** `queue.h:529-537`

```cpp
KJ_CASE_ONEOF(ready, Ready) {
  // There's no reason to GC visit the promise resolver or buffer here and it is
  // potentially problematic if we do. Since the read requests are queued, if we
  // GC visit it once, remove it from the queue, and GC happens to kick in before
  // we access the resolver, then v8 could determine that the resolver or buffered
  // entries are no longer reachable via tracing and free them before we can
  // actually try to access the held resolver.
}
```

**Why it's good:** Documents a subtle GC interaction and explains why the obvious pattern (visiting everything) would be wrong. This kind of documentation is valuable.

---

## 8. Recommendations Summary

### High Priority (Performance Impact)

| Issue | Location | Effort | Impact |
|-------|----------|--------|--------|
| Optimize maybeRunAlgorithm | standard.c++ | Medium | High - hot path |
| Single-allocation read for known sizes | internal.c++ | Low | Medium - common case |
| Consolidate backpressure update | internal.c++ | Low | Low - code quality |

### Medium Priority (Code Quality)

| Issue | Location | Effort | Impact |
|-------|----------|--------|--------|
| Extract state machine visitor | common.h | Medium | Medium - reduces duplication |
| Standardize error creation | All files | High | Medium - maintainability |
| Error message constants | common.h | Low | Low - minor size reduction |

### Low Priority (Nice to Have)

| Issue | Location | Effort | Impact |
|-------|----------|--------|--------|
| Boolean parameter enums | All files | High | Low - readability |
| Function splitting | internal.c++ | Medium | Low - maintainability |
| IoContext helper | common.h | Low | Low - reduces boilerplate |

---

## Appendix: Line Count by Issue Type

| Category | Estimated Duplicated Lines | Files Affected |
|----------|---------------------------|----------------|
| Backpressure update | 10 | internal.c++ |
| Reader/Writer release | 40 | internal.c++, standard.c++ |
| State switch patterns | 200+ | All |
| Error strings | 50 | All |
| IoContext checks | 60 | All |

**Total Estimated Redundancy:** ~360 lines that could be consolidated

---

## Document Information

| Property | Value |
|----------|-------|
| Files Analyzed | internal.c++, standard.c++, queue.c++, queue.h, common.h, writable.h, readable.h |
| Analysis Focus | Efficiency, duplication, patterns |
| Excludes | Micro-optimizations the compiler handles |
| Sections | 8 (including positive patterns) |
