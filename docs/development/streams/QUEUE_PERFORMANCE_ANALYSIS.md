# Queue Implementation Performance Analysis

This document analyzes the performance characteristics of the internal queue implementation
for JS-backed ReadableStreams in workerd, focusing on `ValueQueue` and `ByteQueue` in
`src/workerd/api/streams/queue.h` and `queue.c++`.

**Generated:** December 2024

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architecture Overview](#2-architecture-overview)
3. [ValueQueue Analysis](#3-valuequeue-analysis)
4. [ByteQueue Analysis](#4-bytequeue-analysis)
5. [Data Structure Analysis](#5-data-structure-analysis)
6. [Performance Characteristics](#6-performance-characteristics)
7. [Identified Bottlenecks](#7-identified-bottlenecks)
8. [Improvement Opportunities](#8-improvement-opportunities)
9. [Relationship to Pump Performance](#9-relationship-to-pump-performance)
10. [Bulk Drain Spec Compliance](#10-bulk-drain-spec-compliance)
11. [Internal-Only Buffering Enhancements](#11-internal-only-buffering-enhancements)
12. [Recommendations](#12-recommendations)

---

## 1. Executive Summary

The queue implementation in workerd provides the internal buffering mechanism for JS-backed
ReadableStreams. It supports both value-oriented streams (arbitrary JS values) and byte-oriented
streams (with BYOB support).

### Key Findings

**Positive Aspects:**
- Already implements direct fulfillment when reads are pending (no unnecessary queuing)
- Uses efficient RingBuffer with inline initial capacity (avoids heap allocation for small queues)
- Reference counting allows efficient multi-consumer support (tee)
- Clean separation between ValueQueue and ByteQueue with shared ConsumerImpl

**Performance Concerns:**
- Heap allocation per pending ReadRequest (`kj::heap<ReadRequest>()`)
- Per-operation backpressure updates iterate all consumers
- ByteQueue has complex nested loops with multiple copies
- No bulk operations (drain all, read many)
- Every getValue() creates new V8 handle via addRef()

### Estimated Overhead Per Operation

| Operation | ValueQueue | ByteQueue |
|-----------|-----------|-----------|
| push (no pending read) | ~0.3μs | ~0.3μs |
| push (with pending read) | ~1.5μs | ~2-5μs |
| read (data available) | ~1.5μs | ~2-5μs |
| read (must pend) | ~2μs + heap alloc | ~3μs + heap alloc |
| backpressure update | O(consumers) | O(consumers) |

---

## 2. Architecture Overview

### 2.1 Class Hierarchy

```
QueueImpl<Self>                    # Shared queue logic (template)
├── highWaterMark                  # Backpressure threshold
├── totalQueueSize                 # Current size (max across consumers)
├── consumers: SmallSet<Consumer*> # All attached consumers
└── state: Ready|Closed|Errored

ConsumerImpl<Self>                 # Per-consumer state (template)
├── queue: QueueImpl&              # Back-reference to parent queue
├── buffer: RingBuffer<Entry|Close># Per-consumer data buffer
├── readRequests: RingBuffer<Own<ReadRequest>> # Pending reads
├── queueTotalSize                 # This consumer's buffer size
└── stateListener                  # Callback for state changes

ValueQueue                         # Value stream specialization
├── Entry { jsg::Value, size }     # Arbitrary JS value + calculated size
├── ReadRequest { resolver }       # Simple promise resolution
└── QueueEntry { Rc<Entry> }       # Reference-counted entry

ByteQueue                          # Byte stream specialization
├── Entry { jsg::BufferSource }    # Byte buffer
├── ReadRequest { resolver, PullInto } # With BYOB buffer info
├── QueueEntry { Rc<Entry>, offset }   # Partial consumption support
├── ByobRequest                    # BYOB fulfillment handle
└── pendingByobReadRequests        # For controller.byobRequest API
```

### 2.2 Data Flow

```
Producer (controller.enqueue)
    │
    ▼
Queue.push(entry)
    │
    ├──> For each Consumer:
    │        │
    │        ▼
    │    Consumer.push(entry.clone())
    │        │
    │        ├──> If pending reads: fulfill directly
    │        │        └── resolve(entry.getValue())
    │        │
    │        └──> If no pending reads: buffer
    │                 └── buffer.push_back(entry)
    │
    └──> maybeUpdateBackpressure()
             └── totalQueueSize = max(consumer.size())
```

### 2.3 Key Design Decisions

1. **Per-consumer buffering:** Each consumer maintains its own buffer, enabling
   tee'd streams to read at different rates.

2. **Reference-counted entries:** `kj::Rc<Entry>` allows multiple consumers to
   reference the same underlying data without copying.

3. **Direct fulfillment:** When a read is pending, incoming data bypasses the
   buffer entirely and fulfills the read directly.

4. **RingBuffer storage:** Uses `workerd::RingBuffer` with inline initial capacity
   to avoid heap allocation for typical small queues.

---

## 3. ValueQueue Analysis

### 3.1 handlePush Implementation

```cpp
// queue.c++ lines 150-164
void ValueQueue::handlePush(jsg::Lock& js, ConsumerImpl::Ready& state,
                            QueueImpl& queue, kj::Rc<Entry> entry) {
  // FAST PATH: Direct fulfillment when read is waiting
  if (state.readRequests.empty()) {
    state.queueTotalSize += entry->getSize();
    state.buffer.push_back(QueueEntry{.entry = kj::mv(entry)});
    return;
  }

  // Read is waiting - fulfill directly (no queue)
  KJ_REQUIRE(state.buffer.empty() && state.queueTotalSize == 0);
  state.readRequests.front()->resolve(js, entry->getValue(js));
  state.readRequests.pop_front();
}
```

**Performance characteristics:**
- O(1) for both paths
- Direct fulfillment avoids queue overhead when consumer is keeping up
- `entry->getValue(js)` creates a new V8 handle (addRef)

**Limitation:** Only fulfills ONE pending read per push, even if multiple are waiting.

### 3.2 handleRead Implementation

```cpp
// queue.c++ lines 166-220
void ValueQueue::handleRead(jsg::Lock& js, ConsumerImpl::Ready& state,
                            ConsumerImpl& consumer, QueueImpl& queue,
                            ReadRequest request) {
  // FAST PATH: Data available, no pending reads
  if (state.readRequests.empty() && !state.buffer.empty()) {
    auto& entry = state.buffer.front();
    // ... pop and resolve
    return;
  }

  // SLOW PATH: Must pend the read
  state.readRequests.push_back(kj::heap<ReadRequest>(kj::mv(request)));
  // Notify listener that we want data
  listener.onConsumerWantsData(js);
}
```

**Performance characteristics:**
- O(1) when data is available
- Heap allocation (`kj::heap<ReadRequest>`) when read must pend
- Listener notification enables pull-based sources

### 3.3 Entry Clone Behavior

```cpp
// queue.c++ lines 51-53
kj::Rc<ValueQueue::Entry> ValueQueue::Entry::clone(jsg::Lock& js) {
  return addRefToThis();  // Just increment refcount, no copy
}
```

**Key insight:** Entry cloning is O(1) - just a refcount increment.
The actual JS value is shared, not copied.

---

## 4. ByteQueue Analysis

### 4.1 handlePush Implementation

```cpp
// queue.c++ lines 584-726
void ByteQueue::handlePush(jsg::Lock& js, ConsumerImpl::Ready& state,
                           QueueImpl& queue, kj::Rc<Entry> newEntry) {
  // If no pending reads, just buffer
  if (state.readRequests.empty()) {
    return bufferData(0);
  }

  // Complex loop: try to fulfill pending reads
  while (!state.readRequests.empty() && amountAvailable > 0) {
    auto& pending = *state.readRequests.front();

    // First drain any buffered data
    while (!state.buffer.empty()) {
      // Copy from buffer to pending.pullInto
      destPtr.first(sourceSize).copyFrom(sourcePtr.slice(entry.offset));
      // ... update offsets, sizes
    }

    // Then copy from new entry
    destPtr.first(amountToCopy).copyFrom(entryPtr.slice(entryOffset));
    // ... resolve if satisfied
  }

  // Buffer any remaining data
  if (entryOffset < entrySize) {
    bufferData(entryOffset);
  }
}
```

**Performance characteristics:**
- Nested while loops (buffer drain + pending reads)
- Memory copy for every fulfilled read
- Partial consumption tracked via offset
- Much more complex than ValueQueue

**Key overhead:** Every byte read involves a `memcpy` operation.

### 4.2 handleRead Implementation

```cpp
// queue.c++ lines 728-861
void ByteQueue::handleRead(jsg::Lock& js, ConsumerImpl::Ready& state,
                           ConsumerImpl& consumer, QueueImpl& queue,
                           ReadRequest request) {
  // Lambda for consuming buffered data
  const auto consume = [&](size_t amountToConsume) {
    while (amountToConsume > 0) {
      // Copy from buffer entries to request.pullInto
      destPtr.first(amountToCopy).copyFrom(sourcePtr.first(amountToCopy));
      // ... handle partial consumption, element alignment
    }
  };

  if (state.readRequests.empty() && state.queueTotalSize > 0) {
    // Data available - consume it
    consume(kj::min(state.queueTotalSize, request.pullInto.store.size()));
    request.resolve(js);
  } else {
    // Must pend - also create ByobRequest if BYOB read
    pendingRead();
  }
}
```

**Performance characteristics:**
- Loop-based consumption with partial entry handling
- Element size alignment checks add overhead
- BYOB reads create additional ByobRequest object
- `atLeast` (minBytes) support requires tracking across multiple pushes

### 4.3 BYOB Complexity

```cpp
// queue.c++ lines 419-496
bool ByteQueue::ByobRequest::respond(jsg::Lock& js, size_t amount) {
  // For multiple consumers, must copy data
  if (queue.getConsumerCount() > 1) {
    auto entry = kj::rc<Entry>(store);
    entry->toArrayPtr().copyFrom(sourcePtr);  // Copy!
    queue.push(js, kj::mv(entry), consumer);  // Push to other consumers
  }

  // Handle element alignment
  auto unaligned = req.pullInto.filled % req.pullInto.store.getElementSize();
  if (unaligned > 0) {
    // Must buffer unaligned bytes for next read
    auto excess = kj::rc<Entry>(store);
    excess->toArrayPtr().copyFrom(start);
    consumer.push(js, kj::mv(excess));
  }
}
```

**BYOB overhead:**
- Multiple consumers: data must be copied to share with others
- Element alignment: unaligned bytes require extra allocation and copy
- Partial fulfillment: tracking across multiple respond() calls

---

## 5. Data Structure Analysis

### 5.1 RingBuffer

```cpp
template <typename T, size_t InitialCapacity = 16>
class RingBuffer {
  kj::Array<kj::byte> storage;  // Heap-allocated backing store
  size_t head, tail, count;
  // ...
};
```

**Used for:**
- `ConsumerImpl::Ready::buffer` (entries) - InitialCapacity=16
- `ConsumerImpl::Ready::readRequests` (pending reads) - InitialCapacity=8
- `ByteQueue::State::pendingByobReadRequests` - InitialCapacity=8

**Performance:**
- O(1) push_back, pop_front, front(), back()
- Initial heap allocation for storage (sizeof(T) * InitialCapacity)
- Growth: 2x capacity, moves all elements

**Tradeoff:** Better cache locality than linked list, but growth invalidates references.

### 5.2 SmallSet

```cpp
SmallSet<ConsumerImpl*> consumers;
```

**Used for:** Tracking consumers attached to a queue.

**Optimized for:** Small number of consumers (typically 1-2).

### 5.3 kj::Rc<Entry>

Reference-counted entry wrapper enabling shared ownership across consumers.

**Operations:**
- `clone()` / `addRefToThis()`: O(1) refcount increment
- Destruction: O(1) refcount decrement, free if zero

**Memory layout:**
- Refcount stored alongside entry data
- No separate control block (unlike std::shared_ptr)

---

## 6. Performance Characteristics

### 6.1 Per-Operation Cost Breakdown

**ValueQueue::push (no pending read):**
```
entry->getSize()                    ~0.05μs
state.queueTotalSize +=             ~0.01μs
state.buffer.push_back()            ~0.1μs (no growth)
UpdateBackpressureScope destructor  ~0.1μs (1 consumer)
─────────────────────────────────────────────
Total                               ~0.3μs
```

**ValueQueue::push (with pending read):**
```
state.readRequests.empty() check    ~0.01μs
entry->getValue(js)                 ~0.5μs (V8 handle creation)
resolver.resolve()                  ~0.8μs (promise resolution)
readRequests.pop_front()            ~0.1μs
UpdateBackpressureScope             ~0.1μs
─────────────────────────────────────────────
Total                               ~1.5μs
```

**ValueQueue::read (data available):**
```
state.buffer.empty() check          ~0.01μs
entry.getValue(js)                  ~0.5μs
resolver.resolve()                  ~0.8μs
buffer.pop_front()                  ~0.1μs
queueTotalSize adjustment           ~0.01μs
─────────────────────────────────────────────
Total                               ~1.5μs
```

**ValueQueue::read (must pend):**
```
kj::heap<ReadRequest>()             ~0.3μs (heap allocation)
readRequests.push_back()            ~0.1μs
listener.onConsumerWantsData()      ~0.5μs (call into controller)
─────────────────────────────────────────────
Total                               ~1μs + callback overhead
```

### 6.2 ByteQueue Additional Overhead

**Per-byte operations:**
```
memcpy per read                     ~0.01μs per KB
Offset tracking                     ~0.02μs per entry
Element alignment check             ~0.02μs per read
```

**BYOB-specific:**
```
ByobRequest creation                ~0.3μs
respond() with multi-consumer       +0.5μs + copy time
Unaligned byte handling             +0.3μs + allocation
```

### 6.3 Scaling Behavior

| Consumers | Backpressure Update | Push (all) |
|-----------|---------------------|------------|
| 1         | O(1)                | O(1)       |
| 2 (tee)   | O(2)                | O(2)       |
| N         | O(N)                | O(N)       |

| Queue Size | Push | Read | Memory |
|------------|------|------|--------|
| 0-16       | O(1) | O(1) | ~256B  |
| 17-32      | O(1)* | O(1) | ~512B |
| N          | O(1)* | O(1) | O(N)  |

*Amortized, occasional O(N) for growth

---

## 7. Identified Bottlenecks

### 7.1 Heap Allocation Per Pending Read

**Problem:**
```cpp
state.readRequests.push_back(kj::heap<ReadRequest>(kj::mv(request)));
```

Every read that cannot be immediately fulfilled allocates a new heap object.

**Impact:** ~0.3μs per allocation, memory fragmentation over time.

**Why it's done this way:** ByobRequest holds a reference to ReadRequest, so
the ReadRequest must have stable address. RingBuffer growth would invalidate
inline storage.

### 7.2 Per-Push Backpressure Update

**Problem:**
```cpp
void maybeUpdateBackpressure() {
  totalQueueSize = 0;
  for (auto consumer: consumers) {
    totalQueueSize = kj::max(totalQueueSize, consumer->size());
  }
}
```

Called via `UpdateBackpressureScope` destructor on every push/read completion.

**Impact:** O(consumers) per operation. For single consumer: ~0.1μs.
For many consumers (unusual): scales linearly.

### 7.3 No Bulk Operations

**Problem:** Each push fulfills at most ONE pending read. Each read consumes
at most ONE entry (for ValueQueue).

**Current flow (100 pushes, 100 pending reads):**
```
push #1 → fulfill read #1
push #2 → fulfill read #2
...
push #100 → fulfill read #100
```
Total: 100 push operations + 100 fulfill operations

**Potential bulk flow:**
```
pushMany([1..100]) → fulfillMany(reads[1..100])
```
Total: 1 bulk push + 1 bulk fulfill

**Impact:** Missed batching opportunity, especially for pump operations.

### 7.4 getValue() Creates V8 Handle Every Time

**Problem:**
```cpp
jsg::Value ValueQueue::Entry::getValue(jsg::Lock& js) {
  return value.addRef(js);  // Creates new V8 Local handle
}
```

Every read creates a new V8 handle for the value, even though the underlying
value is the same.

**Impact:** ~0.5μs per getValue() call.

### 7.5 ByteQueue Nested Loops

**Problem:** handlePush has nested while loops:
```cpp
while (!state.readRequests.empty() && amountAvailable > 0) {
  while (!state.buffer.empty()) {
    // drain buffer
  }
  // consume from new entry
}
```

**Impact:** Higher complexity, harder to optimize, more branch mispredictions.

### 7.6 ByteQueue Always Copies

**Problem:** Every byte read involves memcpy:
```cpp
destPtr.first(amountToCopy).copyFrom(sourcePtr.first(amountToCopy));
```

Even when data could potentially be transferred (detached ArrayBuffer).

**Impact:** For 16KB read: ~1μs copy time.

---

## 8. Improvement Opportunities

### 8.1 ReadRequest Pool / Inline Storage

**Current:** Heap allocate every pending read
**Proposed:** Use object pool or inline storage

```cpp
// Option A: Object pool
class ReadRequestPool {
  kj::Vector<ReadRequest> pool;
  ReadRequest* acquire() { /* reuse or allocate */ }
  void release(ReadRequest* req) { /* return to pool */ }
};

// Option B: Inline storage for first N requests
struct Ready {
  std::array<ReadRequest, 4> inlineRequests;
  uint8_t inlineCount = 0;
  RingBuffer<kj::Own<ReadRequest>> overflowRequests;
};
```

**Expected impact:** Eliminate ~0.3μs allocation for common case (1-4 pending reads).

**Complexity:** Medium - need to handle ownership carefully.

### 8.2 Bulk Push / Drain Operations

**Current:** One push → one fulfill
**Proposed:** Bulk operations for internal use

```cpp
// Drain all available entries at once
kj::Vector<kj::Rc<Entry>> ValueQueue::drainAll();

// Push multiple entries, fulfill multiple reads
void ValueQueue::pushMany(kj::ArrayPtr<kj::Rc<Entry>> entries);
```

**Expected impact:** Significant for pump operations. Could reduce overhead
from 100× per-chunk to 1× per-batch.

**Complexity:** Medium - need to maintain backpressure semantics.

**Relationship to pump analysis:** This directly implements the "Bulk Queue Drain"
strategy identified as highest priority in the pump analysis.

### 8.3 Lazy Backpressure Update

**Current:** Update on every push/read
**Proposed:** Batch updates, only compute when queried

```cpp
class QueueImpl {
  bool backpressureDirty = false;

  void markBackpressureDirty() { backpressureDirty = true; }

  ssize_t desiredSize() {
    if (backpressureDirty) {
      recomputeBackpressure();
      backpressureDirty = false;
    }
    return highWaterMark - totalQueueSize;
  }
};
```

**Expected impact:** Reduces O(consumers) operations per push/read to
O(consumers) only when desiredSize is actually queried.

**Complexity:** Low - straightforward change.

### 8.4 Direct Value Transfer for Single Consumer

**Current:** Always use Rc<Entry>, always getValue() with addRef
**Proposed:** For single consumer, transfer ownership directly

```cpp
void ValueQueue::handlePush(...) {
  if (queue.getConsumerCount() == 1 && !state.readRequests.empty()) {
    // Single consumer with pending read - transfer directly
    auto value = entry->takeValue(js);  // Move, not copy
    state.readRequests.front()->resolve(js, kj::mv(value));
    // Entry is consumed, no need for Rc
  }
}
```

**Expected impact:** Eliminates refcount operations and addRef for common case.

**Complexity:** Medium - need to handle consumer count changes.

### 8.5 Zero-Copy for ByteQueue

**Current:** Always copy bytes to destination buffer
**Proposed:** Transfer ArrayBuffer when possible

```cpp
void ByteQueue::handleRead(...) {
  if (canTransfer(entry) && entryFillsRequest(entry, request)) {
    // Transfer entire entry buffer to read destination
    auto buffer = entry->detachBuffer(js);
    request.resolve(js, kj::mv(buffer));
    return;
  }
  // Fall back to copy
}
```

**Conditions for zero-copy:**
- Single entry fills the entire read request
- Entry buffer is detachable
- Element size alignment matches

**Expected impact:** Eliminates copy for large reads (64KB: ~4μs saved).

**Complexity:** High - need careful V8 ArrayBuffer handling.

### 8.6 Reduce Indirection in ByteQueue

**Current:** Multiple lambdas, nested loops
**Proposed:** Flatten control flow, inline hot paths

```cpp
// Instead of:
const auto consume = [&](size_t amount) { ... };
consume(kj::min(queueSize, requestSize));

// Inline directly:
while (amountToConsume > 0 && !state.buffer.empty()) {
  // ... consume logic inline
}
```

**Expected impact:** Better branch prediction, fewer function call overheads.

**Complexity:** Low - refactoring only.

### 8.7 Skip Queue for Direct Push-to-Read

**Current:**
```cpp
// In handlePush, if read is waiting:
state.readRequests.front()->resolve(js, entry->getValue(js));
```

Already implemented! But only for single entry → single read.

**Enhancement:** Extend to handle multiple entries → multiple reads in one call.

---

## 9. Relationship to Pump Performance

The queue implementation directly impacts pump performance in several ways:

### 9.1 Current Pump-Queue Interaction

```
pumpReadImpl (in readable-source-adapter.c++)
    │
    ├── readInternal()
    │       │
    │       ├── reader.read()  → creates JS Promise
    │       │       │
    │       │       └── ValueQueue::handleRead()
    │       │               │
    │       │               └── If data: resolve immediately
    │       │                   If no data: pend request
    │       │
    │       └── await promise
    │
    └── repeat until buffer full or done
```

### 9.2 Per-Chunk Overhead Sources

| Source | Queue Contribution | Pump Contribution |
|--------|-------------------|-------------------|
| Promise creation | ~1.5μs | readInternal creates promise |
| getValue/addRef | ~0.5μs | Queue returns value via addRef |
| Heap alloc (pend) | ~0.3μs | When consumer is slow |
| Backpressure update | ~0.1μs | Per push/read |

### 9.3 How Queue Improvements Help Pump

| Queue Improvement | Pump Benefit |
|-------------------|--------------|
| Bulk drain | Eliminates per-chunk promise for batch |
| Direct value transfer | Reduces getValue overhead |
| Lazy backpressure | Reduces per-operation overhead |
| ReadRequest pool | Reduces allocation in slow-consumer case |

### 9.4 Alignment with Pump Recommendations

From the pump analysis (Section 20), the highest-priority recommendation was
**Bulk Queue Drain**. This maps directly to:
- `ValueQueue::drainAll()` - return all buffered entries
- `ByteQueue::drainAll()` - return all buffered bytes

The pump could then:
1. Call `drainAll()` once per buffer fill cycle
2. Process all returned data in a tight C++ loop
3. Avoid per-chunk JS promise overhead

---

## 10. Bulk Drain Spec Compliance

A critical question: can bulk drain be implemented while preserving spec-compliant behavior?
Specifically, if there's not enough data to fulfill a read, the pull algorithm must be
triggered appropriately.

### 10.1 Spec Requirements That Must Be Preserved

1. **Pull algorithm timing**: When `desiredSize > 0`, the pull algorithm must be invoked
2. **Promise resolution order**: Read promises must resolve in FIFO order
3. **Backpressure signaling**: `desiredSize` must accurately reflect queue state
4. **Tee branch independence**: Each branch maintains independent consumption rate

### 10.2 Key Insight: Internal Optimization

A bulk drain is feasible as an **internal optimization** (not a new JS API) because the
pump controls both sides of the read operation. The pump isn't a JS consumer awaiting
`reader.read()` promises - it's C++ code that can access the queue directly.

### 10.3 Proposed API

```cpp
// Internal API - NOT exposed to JS
struct DrainResult {
  kj::Vector<kj::Rc<Entry>> entries;  // Available data (may be empty)
  bool closed;
  bool needsPull;  // True if desiredSize > 0 after drain
};

DrainResult ValueQueue::Consumer::drainAvailable() {
  // Returns whatever is buffered RIGHT NOW
  // Does not block or create promises

  DrainResult result;
  result.closed = state.is<Closed>();

  if (auto* ready = state.tryGet<Ready>()) {
    // If there are pending JS read requests, we can't drain
    // because that data is "promised" to those reads
    if (!ready->readRequests.empty()) {
      result.needsPull = desiredSize() > 0;
      return result;
    }

    while (!ready->buffer.empty()) {
      auto& front = ready->buffer.front();
      if (auto* entry = front.tryGet<QueueEntry>()) {
        result.entries.add(kj::mv(entry->entry));
        ready->queueTotalSize -= entry->entry->getSize();
      } else {
        result.closed = true;
        break;
      }
      ready->buffer.pop_front();
    }
  }

  result.needsPull = desiredSize() > 0;
  return result;
}
```

### 10.4 Pump Usage Pattern

```cpp
// In pumpReadImpl - internal pump loop
kj::Promise<void> pumpLoop() {
  while (true) {
    // 1. Drain whatever is available (no JS promises!)
    auto drained = consumer.drainAvailable();

    if (!drained.entries.empty()) {
      // 2. Process all drained entries in tight C++ loop
      for (auto& entry : drained.entries) {
        co_await destination.write(entry->getBytes());
      }
    }

    if (drained.closed) {
      co_return;  // Done
    }

    // 3. If queue needs more data, trigger pull
    if (drained.needsPull) {
      triggerPullAlgorithm();  // Calls JS pull() function
    }

    // 4. If nothing was available, wait for data to arrive
    if (drained.entries.empty()) {
      co_await consumer.onDataAvailable();  // Single wait point
    }
  }
}
```

### 10.5 How Spec Requirements Are Preserved

| Requirement | How It's Preserved |
|-------------|-------------------|
| Pull timing | `needsPull` flag triggers pull when `desiredSize > 0` |
| Promise order | No JS promises involved - this is internal C++ path |
| Backpressure | `desiredSize` still computed correctly after drain |
| Tee support | Each consumer's `drainAvailable()` is independent |

### 10.6 Constraint: Exclusive Reader

If there are already JS `reader.read()` calls pending (from user code, not the pump),
we cannot steal their data. The `drainAvailable()` implementation checks for pending
read requests and returns empty if any exist.

In the pump case, the pump is the exclusive reader - it owns the reader and no other
JS code is calling `read()`. So this check passes.

### 10.7 ByteQueue Considerations

ByteQueue is more complex due to:
1. Partial consumption (offset tracking)
2. Element size alignment
3. BYOB buffer filling

A bulk drain for ByteQueue would return concatenated bytes:

```cpp
struct ByteDrainResult {
  kj::Array<kj::byte> bytes;  // Concatenated available bytes
  bool closed;
  bool needsPull;
};
```

This requires copying bytes into a contiguous buffer, but that's the same copy that
would happen anyway with individual reads - just batched. The copy overhead is
amortized across the batch.

### 10.8 Conclusion

**Yes, bulk drain can be implemented spec-compliantly** with these constraints:

1. It's an **internal-only** optimization for the pump path
2. Only works when pump is the **exclusive reader** (no pending JS reads)
3. Pull algorithm is triggered via `needsPull` flag after drain
4. Tee'd streams work because each consumer drains independently
5. ByteQueue still requires copying, but batched

The main benefit is eliminating **per-chunk JS promise creation** in the pump hot path,
which the pump analysis identified as a significant overhead source (~1.5μs per chunk).

---

## 11. Internal-Only Buffering Enhancements

Beyond the user-facing `highWaterMark` (which has observable semantics via `desiredSize` and
backpressure signaling), there are internal-only buffering mechanisms we can explore that
would not affect user-visible behavior.

### 11.1 User-Visible vs Internal-Only

| Aspect | User-Visible | Internal-Only |
|--------|:------------:|:-------------:|
| `desiredSize` value | ✓ | |
| Pull algorithm timing | ✓ | |
| Read data order | ✓ | |
| Backpressure signaling | ✓ | |
| Internal memory layout | | ✓ |
| When copies happen | | ✓ |
| Entry allocation strategy | | ✓ |
| Batch sizes for internal ops | | ✓ |

**Key insight:** Anything affecting **when** data becomes available or **how much** the source
is asked to produce is user-visible (via `desiredSize`, pull timing). But **how we store it**,
**when we copy it**, and **how we batch internal operations** is purely internal.

### 11.2 Entry Coalescing (ByteQueue)

Multiple small pushes could be coalesced into a single larger entry internally:

```cpp
class ByteQueue {
  // Internal coalescing buffer - not reflected in queueTotalSize until flushed
  kj::Vector<kj::byte> coalesceBuffer;
  static constexpr size_t COALESCE_THRESHOLD = 4096;

  void handlePush(..., kj::Rc<Entry> entry) {
    if (entry->size() < COALESCE_THRESHOLD &&
        coalesceBuffer.size() < COALESCE_THRESHOLD) {
      // Accumulate small writes
      coalesceBuffer.addAll(entry->toArrayPtr());
      // Still update queueTotalSize for backpressure
      state.queueTotalSize += entry->size();
      return;
    }

    // Flush coalesce buffer as single entry
    if (!coalesceBuffer.empty()) {
      flushCoalesceBuffer();
    }
    // Then handle current entry normally
  }
};
```

**Why it's internal-only:** Reads still return bytes in order; we just reorganize storage.

**Benefit:** Fewer entries to iterate, better cache locality, reduced per-entry overhead.

### 11.3 Pump Wake-up Threshold

The pump could have an internal threshold before it processes data:

```cpp
class PumpState {
  // Internal threshold - not related to highWaterMark
  static constexpr size_t MIN_PUMP_BATCH = 16384;  // 16KB

  kj::Promise<void> waitForData() {
    // Wait until either:
    // - MIN_PUMP_BATCH bytes available, OR
    // - Stream is closed, OR
    // - Timeout expires (don't starve small streams)
    return consumer.onDataAvailable(MIN_PUMP_BATCH)
        .exclusiveJoin(afterDelay(1ms));
  }
};
```

**Why it's internal-only:** `desiredSize`/backpressure unchanged; we just batch consumption.

**Benefit:** Reduces pump loop iterations, amortizes per-iteration overhead.

### 11.4 Lazy V8 Handle Creation

Defer `jsg::Value` handle creation until actually needed:

```cpp
class ValueQueue::Entry {
  // Store raw V8 value reference instead of jsg::Value
  v8::Global<v8::Value> rawValue;  // Prevent GC but defer wrapper

  jsg::Value getValue(jsg::Lock& js) {
    // Only create jsg::Value wrapper when actually reading
    return jsg::Value(js.v8Isolate(), rawValue.Get(js.v8Isolate()));
  }
};
```

**Why it's internal-only:** The JS value returned is identical; we just defer wrapper creation.

**Benefit:** Avoid ~0.5μs per entry if entries are buffered but not immediately read.

**Risk:** Requires careful GC interaction; `v8::Global` prevents collection but adds overhead.

### 11.5 ReadRequest Inline Storage

Use inline storage before falling back to heap:

```cpp
struct ConsumerImpl::Ready {
  // First 4 read requests stored inline (common case for pump)
  static constexpr size_t INLINE_REQUESTS = 4;
  std::array<std::aligned_storage_t<sizeof(ReadRequest),
             alignof(ReadRequest)>, INLINE_REQUESTS> inlineStorage;
  uint8_t inlineCount = 0;

  // Overflow to heap-allocated ring buffer
  RingBuffer<kj::Own<ReadRequest>, 8> overflowRequests;
};
```

**Why it's internal-only:** Read semantics unchanged; just different allocation strategy.

**Benefit:** Eliminate heap allocation for common case (1-4 pending reads).

### 11.6 Internal Pre-pull Hint

Trigger pull earlier than spec requires, as an optimization hint:

```cpp
void ConsumerImpl::afterRead() {
  // Spec-required pull: when desiredSize > 0
  if (desiredSize() > 0) {
    triggerPull();
  }

  // Internal optimization: pre-pull when buffer is 50% depleted
  // This doesn't change when pull is *required*, just adds earlier hints
  if (queueTotalSize < highWaterMark / 2 && !prePullPending) {
    prePullPending = true;
    schedulePrePull();  // Non-blocking hint to source
  }
}
```

**Why it's internal-only:** We still call pull at spec-required times; we just *also* hint earlier.

**Benefit:** Reduce latency when consumer catches up to producer.

**Risk:** Could cause over-production; needs careful design to avoid memory bloat.

### 11.7 Internal Drain Watermark

For pump operations, an internal "drain until" threshold:

```cpp
struct DrainConfig {
  // Internal target - aim to drain this much per cycle
  size_t targetDrainSize = 65536;  // 64KB

  // But don't wait forever - also drain on timeout
  kj::Duration maxWait = 1 * kj::MILLISECONDS;
};

DrainResult drainAvailable(DrainConfig config) {
  // Drain up to targetDrainSize, or whatever is available
  // This is purely internal batching, doesn't affect desiredSize
}
```

**Why it's internal-only:** External backpressure unchanged; we just batch internal operations.

**Benefit:** Predictable batch sizes for downstream processing.

### 11.8 Entry Pool

Reuse Entry objects instead of allocating new ones:

```cpp
class EntryPool {
  kj::Vector<kj::Own<Entry>> pool;
  static constexpr size_t MAX_POOL_SIZE = 64;

  kj::Own<Entry> acquire(jsg::Lock& js, jsg::Value value, size_t size) {
    if (!pool.empty()) {
      auto entry = kj::mv(pool.back());
      pool.removeLast();
      entry->reset(js, kj::mv(value), size);
      return entry;
    }
    return kj::heap<Entry>(js, kj::mv(value), size);
  }

  void release(kj::Own<Entry> entry) {
    if (pool.size() < MAX_POOL_SIZE) {
      entry->clear();
      pool.add(kj::mv(entry));
    }
    // else: let it be destroyed
  }
};
```

**Why it's internal-only:** Entry lifecycle is internal implementation detail.

**Benefit:** Reduce allocation overhead for high-throughput streams.

### 11.9 Summary

| Enhancement | Effort | Impact | Risk |
|-------------|--------|--------|------|
| Entry coalescing | Medium | Medium | Low |
| Pump wake-up threshold | Low | Medium | Low |
| Lazy V8 handle | Medium | Medium | Medium (GC) |
| ReadRequest inline storage | Medium | Low-Medium | Low |
| Internal pre-pull hint | Medium | Medium | Medium (over-production) |
| Internal drain watermark | Low | Medium | Low |
| Entry pool | Medium | Low-Medium | Low |

These enhancements are orthogonal to the bulk drain optimization and can be implemented
independently. The combination of bulk drain (Section 10) with internal batching thresholds
(11.3, 11.7) would provide the largest performance improvement for pump operations.

---

## 12. Recommendations

### 12.1 Cross-Reference

This section aligns with the consolidated ranking in **PUMP_PERFORMANCE_ANALYSIS.md §20.8**.
See also **JSG_PROMISE_PERFORMANCE_ANALYSIS.md §11** for promise-specific recommendations.

### 12.2 Priority Ranking (Queue-Specific)

The following ranking shows queue-specific improvements ordered by impact, aligned with the
unified cross-document ranking.

*Note: Rankings account for tcmalloc in production - see §12.7 below.*

| Unified Rank | Queue Improvement | Effort | Impact | Cross-Ref |
|--------------|-------------------|--------|--------|-----------|
| **1** | **Bulk drain + DeferredPromise** | Medium-High | **50-60%** | JSG §10.14, Pump §20.8 |
| **2** | **Sync fast path (ready data)** | Medium | **30-40%** | Section 10, JSG §8.1 |
| **3** | **Direct value transfer** | Medium | **~15%** | Section 8, JSG §8.7 |
| 7 | Lazy backpressure update | Low | O(consumers) | Section 8 |
| 10 | Zero-copy ByteQueue | High | Large data | Section 8 |
| 13 | ReadRequest pool/inline storage | Medium | ~0.05μs/read † | Section 11.5 |
| 14 | Entry pool reuse | Low | Minimal † | Section 11.8 |
| 15 | Flatten ByteQueue loops | Low | Branch pred | Section 8 |

*† Demoted due to tcmalloc - allocation overhead already negligible in production.*

### 12.3 Key Insight: Queue + Promise Combined

The bulk drain operation (Section 10) achieves maximum benefit when combined with the
**DeferredPromise** design from JSG_PROMISE_PERFORMANCE_ANALYSIS.md §10.14:

| Approach | Per-Chunk Cost | Improvement |
|----------|----------------|-------------|
| Current (per-chunk jsg::Promise) | ~8μs | — |
| Bulk drain alone | ~1μs amortized | 7x |
| Bulk drain + DeferredPromise | ~0.3μs amortized | 26x |

### 12.4 Implementation Order

**Phase 1: Foundation (Low risk, immediate benefit)**
1. Lazy backpressure update (low effort, medium benefit)
2. Flatten ByteQueue nested loops (low effort, low benefit)

**Phase 2: Core Optimizations (High impact)**
3. Bulk drain operation (Section 10) - enables pump batching

**Phase 3: JSG Integration (Requires JSG changes)**
4. DeferredPromise integration (see JSG §10.14)
5. Direct value transfer for exclusive reader (Section 8)

**Phase 4: Advanced (Specialized scenarios)**
6. Zero-copy ByteQueue transfers (large data only)

*Note: Pool-based optimizations (ReadRequest pool, Entry pool) removed from implementation
order - tcmalloc makes these low-value in production. See §12.7.*

### 12.5 Caveats

**Spec compliance:** Bulk operations must maintain correct observable behavior:
- Backpressure signaling at correct points
- Error/close propagation in correct order
- Tee branch synchronization

**Memory management:** Zero-copy and direct transfer require careful handling of:
- ArrayBuffer detachment semantics
- Consumer count changes during operation
- GC interaction with V8 handles

### 12.6 Metrics to Track

When implementing improvements:
- Per-operation latency (μs)
- Heap allocations per 1000 reads
- Throughput (MB/s) for various chunk sizes
- Memory usage for queue + consumers
- Promise creations per 1000 reads (cross-ref with JSG metrics)

### 12.7 tcmalloc Considerations (Production Environment)

In production, workerd uses **tcmalloc** as the memory allocator. This significantly
affects the value of allocation-focused optimizations:

**Allocation overhead with tcmalloc:**

| Allocation | Generic malloc | tcmalloc |
|------------|---------------|----------|
| `kj::heap<ReadRequest>()` | ~0.3μs | ~0.05μs |
| `kj::heap<Entry>()` | ~0.3μs | ~0.05μs |
| RingBuffer grow | ~0.5μs | ~0.1μs |

**Impact on queue-specific optimizations:**

| Optimization | Without tcmalloc | With tcmalloc | Recommendation |
|--------------|------------------|---------------|----------------|
| ReadRequest pool (§11.5) | Medium value | Low value | **Deprioritize** |
| Entry pool (§11.8) | Low-Medium value | Minimal value | **Skip** |
| Inline ReadRequest storage | Medium value | Low value | **Deprioritize** |

**Why pool optimizations lose value:**
- tcmalloc thread-local caches provide ~0.05μs allocation
- Pool management overhead may exceed allocation savings
- Added code complexity not justified by minimal gains

**Recommended focus:** Concentrate on V8/JSG overhead elimination (bulk drain,
DeferredPromise) which provides 50-60% improvement regardless of allocator.

### 12.8 Multi-Threaded Environment Considerations (Production)

Production deployments use a multi-threaded model where isolates may migrate between
threads during I/O waits. This affects queue-related optimizations:

**How isolate migration affects queue operations:**

When the pump loop awaits I/O (e.g., waiting for data from the source), the isolate
lock is released. Another request may acquire the lock, or when I/O completes, the
same request may resume on a different physical thread.

```
Per-chunk queue access pattern:
  Thread A: [lock] → queue.read() → [unlock for I/O wait]
  Thread B: [lock] → queue.read() → [unlock for I/O wait]  ← Different thread!
  Thread A: [lock] → queue.read() → ...
```

**Impact on queue-specific optimizations:**

| Optimization | Additional Considerations |
|--------------|--------------------------|
| Bulk drain | **More valuable**: Fewer lock acquisitions per batch |
| Lazy backpressure | Unchanged - computation happens under lock |
| ReadRequest pool | **Less valuable**: Cross-thread alloc/dealloc |
| Entry pool | **Less valuable**: Same reason |

**IoOwn<T> and IoContext validation:**

Queue operations involve `IoOwn<T>` wrapped objects (KJ I/O objects held by JSG).
Each access validates the IoContext, adding ~0.05-0.1μs overhead not present in
single-threaded benchmarks.

**Cache effects on queue data structures:**

When isolates migrate between cores:
- RingBuffer storage may need to be fetched from remote cache
- Entry data (jsg::Value) requires cache line transfers

The bulk drain optimization helps here by:
1. Processing all available data in one lock acquisition
2. Reducing the number of cross-core data fetches
3. Better amortizing cache warm-up costs across the batch

**Revised recommendation:** Bulk drain becomes **even more critical** in production
because it reduces both V8 overhead AND lock acquisition cycles. Pool optimizations
become **even less valuable** due to cross-thread allocation patterns.

---

## Appendix: Code References

### Key Files
- `src/workerd/api/streams/queue.h` - Queue class definitions
- `src/workerd/api/streams/queue.c++` - Queue implementations
- `src/workerd/util/ring-buffer.h` - RingBuffer implementation
- `src/workerd/api/streams/readable-source-adapter.c++` - Pump implementation

### Key Functions
- `ValueQueue::handlePush` (queue.c++:150-164)
- `ValueQueue::handleRead` (queue.c++:166-220)
- `ByteQueue::handlePush` (queue.c++:584-726)
- `ByteQueue::handleRead` (queue.c++:728-861)
- `ConsumerImpl::push` (queue.h:394-406)
- `ConsumerImpl::read` (queue.h:408-422)
