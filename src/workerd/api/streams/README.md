# Streams Implementation Reference

Terse reference for the streams subsystem. For narrative tutorial, see
[docs/streams.md](../../../../docs/streams.md).

For file map and coding invariants, see [AGENTS.md](AGENTS.md).

## Classification Matrix

| Axis             | Internal                                      | Standard                            |
| ---------------- | --------------------------------------------- | ----------------------------------- |
| Spec conformance | Non-standard (kj-backed)                      | WHATWG Streams                      |
| Data types       | Byte-only (TypedArray/ArrayBuffer)            | Byte or Value (any JS value)        |
| Queue model      | No queue; single pending read                 | Dual queues (data + pending reads)  |
| Async model      | kj::Promise / kj event loop                   | JS Promise / microtasks             |
| Isolate lock     | Data flows outside lock                       | Data flows inside lock              |
| Backpressure     | Implicit (kj flow control)                    | Explicit (highwater mark + size fn) |
| Reader types     | Default + BYOB                                | Default + BYOB (byte streams only)  |
| Readable ctrl    | `ReadableStreamInternalController`            | `ReadableStreamJsController`        |
| Writable ctrl    | `WritableStreamInternalController`            | `WritableStreamJsController`        |
| Readable backing | `ReadableStreamSource` (kj::AsyncInputStream) | JS pull/cancel algorithms           |
| Writable backing | `WritableStreamSink` (kj::AsyncOutputStream)  | JS write/abort/close algorithms     |
| Creation         | `request.body`, internal APIs                 | `new ReadableStream({...})`         |

Note: the `ReadableStream`/`WritableStream` APIs provide no way to inspect whether a
stream is byte-oriented or value-oriented at runtime.

## Standard ReadableStream

### Queue Mechanics

Two queues: **data queue** (chunks waiting to be read) and **pending read queue**
(unfulfilled read requests).

Four algorithms: **start** (init), **pull** (request data), **cancel** (abort),
**size** (backpressure calculation).

Highwater mark rules:

- Pull is invoked when data queue size < highwater mark
- Pull pushes data in; if a pending read exists, it is fulfilled immediately
- If no pending read, data goes to the queue
- If enqueued data exceeds what the first pending read needs, excess goes to queue;
  remaining pending reads drain against queued data

```
        +----------------+
        | pull algorithm | <------------------------------------------+
        +----------------+                                            |
                |                                                     |
                v                                                     |
        +---------------+                                             |
        | enqueue(data) |                                             |
        +---------------+                                             |
                |                                                     |
                v                                                     |
      +--------------------+       +-------------------+              |
      | has a pending read | ----> | has data in queue |              |
      +--------------------+  yes  +-------------------+              |
                |                     |      no|                      |
              no|                     |        v                      |
                v                     |  +----------------------+     |
       +-------------------+    yes   |  | fulfill pending read |     |
       | add data to queue | <--------+  +----------------------+     |
       +-------------------+                    |                     |
                       |                        |                     |
                       |                        v                     |
                       |    +-----------------------------+     no    |
                       +--> | is queue at highwater mark? | -----------
                            +-----------------------------+
                                      yes |
                                          v
                                        (done)
```

On `reader.read()`:

- Queue has data -> fulfill immediately; if queue drops below HWM, call pull
- Queue empty -> add to pending read queue; if below HWM, call pull

```
    +---------------+
    | reader.read() |
    +---------------+
           |
           v
   +-----------------+       +------------------+       +---------------------+
   | queue has data? | ----> | add pending read | ----> | call pull algorithm |
   +-----------------+  no   +------------------+       +---------------------+
           |
       yes |
           v
    +--------------+
    | fulfill read |
    +--------------+
           |
           v
         (done)
```

**WARNING**: Source can push data even with no readers and after backpressure signal.
Queue grows unbounded. User code can enqueue via controller reference at any time,
independent of pull algorithm.

### Tee Behavior

Non-standard optimization: branches hold `kj::Rc<Entry>` shared references instead
of copying data. Backpressure to trunk based on branch with most unconsumed data.

```
   +----------------+
   | pull algorithm |
   +----------------+
           |
           v          ..........................................................
   +---------------+  .   +---------------------+      +-------------------+
   | enqueue(data) | ---> | push data to branch | ---> | has pending read? |
   +---------------+  .   +---------------------+      +-------------------+
               |      .                                no |    yes |
               |      .       +-------------------+       |  +--------------+
               |      .       | add data to queue | <-----+  | fulfill read |
               |      .       +-------------------+          +--------------+
               |      ............................................................
               |      .   +---------------------+      +-------------------+
               +--------> | push data to branch | ---> | has pending read? |
                      .   +---------------------+      +-------------------+
                      .                                no |    yes |
                      .       +-------------------+       |  +--------------+
                      .       | add data to queue | <-----+  | fulfill read |
                      .       +-------------------+          +--------------+
                      ............................................................
```

Does not completely prevent slow-branch buildup but avoids memory pileup when source
respects backpressure signals.

## Internal ReadableStream

No queue. No pull algorithm. Single pending read only (second `read()` throws).
Backed by `ReadableStreamSource` wrapping `kj::AsyncInputStream`.

`read()` -> `tryRead()` on source -> `kj::Promise<data>`. Data flows only when a
reader is active. Each read bounded by max byte count.

```
    +---------------+
    | reader.read() |
    +---------------+
            |
            v
  +-------------------+      +-----------------------------------+
  | has pending read? | ---> | tryRead() on ReadableStreamSource |
  +-------------------+  no  +-----------------------------------+
        yes |
            v
        +-------+
        | error |
        +-------+
```

## Standard WritableStream

Five algorithms: **start**, **write**, **abort**, **close**, **size**.

`writer.write(data)` invokes write algorithm; promise resolves on completion.
Backpressure signals:

- `writer.desiredSize` -- space remaining before queue full
- `writer.ready` -- promise resolved when backpressure clears; replaced with
  new promise on each backpressure signal

Highwater mark is advisory. Write algorithm must accept calls regardless of
current mark value.

## Internal WritableStream

Backed by `WritableStreamSink` wrapping `kj::AsyncOutputStream`.
`write()` passes directly to sink. Behavior depends on specific sink implementation.

## IdentityTransformStream State Machine

Byte-only (`ArrayBuffer`/`TypedArray`). Single shared class implements both
`ReadableStreamSource` and `WritableStreamSink`.

Compat flag: when enabled, `new TransformStream()` creates Standard TransformStream.
When disabled, `new TransformStream()` aliases `new IdentityTransformStream()`.

| Current State | Event     | Next State    | Action                                |
| ------------- | --------- | ------------- | ------------------------------------- |
| Idle          | `write()` | Write Pending | Hold data; return pending promise     |
| Idle          | `read()`  | Read Pending  | Return pending promise                |
| Read Pending  | `write()` | Idle          | Fulfill read with data; resolve write |
| Write Pending | `read()`  | Idle          | Fulfill read with held data           |

Only one in-flight read or write at a time. `write()` promise won't resolve until
a corresponding `read()`, and vice versa.

## Standard TransformStream

Three algorithms: **start**, **transform**, **flush**.

`write()` -> transform algorithm -> `controller.enqueue(result)` -> readable side queue.
Both sides are full Standard ReadableStream/WritableStream with independent queues and
backpressure. Writes not blocked on reads unless backpressure signals queue full.
Any JS value, not byte-restricted.

## Pipe Loop Selection

Entry point: `writableController.tryPipeFrom(readable)` -- destination selects loop type.

| Readable | Writable | Loop Type | Isolate Lock | Data Restriction |
| -------- | -------- | --------- | ------------ | ---------------- |
| Internal | Internal | kj-to-kj  | Not held     | Bytes only       |
| Internal | Standard | kj-to-JS  | Held         | Bytes only       |
| Standard | Internal | JS-to-kj  | Held         | Bytes only       |
| Standard | Standard | JS-to-JS  | Held         | Any value        |

```
         +---------------------------+
         | readable.pipeTo(writable) |
         +---------------------------+
                       |
                       v
   +------------------------------------------+
   | writableController.tryPipeFrom(readable) |
   +------------------------------------------+
                       |
                       v
           +-----------------------+       +-----------------------+
           | is internal writable? | ----> | is internal readable? |
           +-----------------------+  yes  +-----------------------+
                       |                      no |         yes |
                    no |                         |             |
                       v                    +----+     +---------------+
           +-----------------------+        |          | kj-to-kj pipe |
           | is internal readable? |        |          |      loop     |
           +-----------------------+        |          +---------------+
            no  |           yes |           |
                |               |           |
                v               |           |
         +---------------+      |           v
         | JS-to-JS pipe |      |   +---------------+
         |      loop     |      |   | JS-to-kj pipe |
         +---------------+      |   |     loop      |
                                v   +---------------+
                        +---------------+
                        | kj-to-JS pipe |
                        |     loop      |
                        +---------------+
```

### kj-to-kj

Fully optimized. kj pipes `AsyncInputStream` to `AsyncOutputStream` via kj event loop.
No JS involvement. Data never enters JS heap.

### kj-to-JS / JS-to-kj

Bridges kj::Promise and JS Promise. Entire flow under isolate lock. Data restricted
to bytes (Standard streams may be value-oriented but API provides no way to inspect,
so byte-only is enforced at the bridge).

### JS-to-JS

Pure JS promise chaining under isolate lock. Equivalent to:
`for await (const chunk of reader) { await writer.write(chunk); }`

## Standard-to-Internal Bridge

APIs like `new Response(standardReadable)` were built for Internal streams and use
kj async I/O. Standard ReadableStreams support consumption via the `ReadableStreamSource`
adapter API (same API Internal streams use).

When `pumpTo()` is called on the adapter:

1. Acquires isolate lock
2. Runs read->write promise loop
3. Ends when data exhausted or stream errors

Implemented in `readable-source-adapter.{h,c++}`.

## Safety Pattern Catalog

### Pattern: State Machine (`StateMachine<>`)

- **When**: All stream controllers
- **What**: Terminal states (`Closed`, `Errored`) prevent double-close/double-error.
  `PendingStates<>` defers transitions during operations. `ActiveState<>` marks
  operational state.
- **Where**: `util/state-machine.h`

### Pattern: Deferred State Transitions

- **When**: Calling code that may invoke JS callbacks during a read/write operation
- **Why**: JS callbacks can trigger close/error mid-operation; state must not change
  until operation completes
- **How**: `beginOperation()` increments counter; `endOperation()` applies pending
  transition when counter hits 0
- **Entry point**: `deferControllerStateChange()` in `standard.h`

```cpp
controller.state.beginOperation();  // Increment counter
auto result = readCallback();       // May trigger JS that calls close()
controller.state.endOperation();    // Apply pending state if counter == 0
```

### Pattern: Consumer Snapshot

- **When**: Iterating over consumers when loop body may trigger JavaScript
- **Why**: JS callbacks during iteration can add/remove consumers, invalidating iterators
- **How**: Copy before iterating

```cpp
auto consumers = ready.consumers.snapshot();
for (auto consumer: consumers) {
    consumer->push(js, entry->clone(js));  // May trigger JS that modifies consumers
}
```

### Pattern: WeakRef for User-Held Handles

- **When**: Handles user code may hold longer than underlying object (`ByobRequest`,
  `PumpToReader`)
- **How**: Check liveness before use

```cpp
KJ_IF_SOME(reader, pumpToReader->tryGet()) {
    reader.pumpLoop(js, ...);  // Safe -- still alive
}
```

### Pattern: `Rc<Entry>` for Shared Queue Data

- **When**: Queue entries shared across teed stream consumers
- **Why**: Prevents use-after-free when one branch consumes before another
- **How**: `kj::Rc<Entry>` reference counting; `entry->clone(js)` per consumer

```cpp
class Entry: public kj::Refcounted {
    kj::Rc<Entry> clone(jsg::Lock& js);
};
```

### Pattern: Lambda Capture Re-check

- **When**: Lambdas attached to promise continuations that may execute after lock release
- **Why**: Referenced objects may be destroyed between capture and execution
- **How**: Re-acquire references inside lambda; check lock state before use
- **Rule**: NEVER capture raw references that may become dangling. Use `addRef()` or
  re-acquire.

```cpp
auto onSuccess = JSG_VISITABLE_LAMBDA((this, ref = addRef(), ...), ..., (...) {
    auto maybePipeLock = lock.tryGetPipe();
    if (maybePipeLock == kj::none) return js.resolvedPromise();
    auto& pipeLock = KJ_REQUIRE_NONNULL(maybePipeLock);
    // Now safe to use pipeLock
});
```

### Pattern: StateListener Self-Destruction Guard

- **When**: Consumer state listener callbacks (`onConsumerClose`, etc.)
- **Why**: Callback may destroy `this` via `owner.doClose()`
- **Rule**: NEVER access `this` after calling methods that may destroy the listener

```cpp
void onConsumerClose(jsg::Lock& js) override {
    KJ_IF_SOME(s, state) {
        s.owner.doClose(js);  // May destroy *this!
    }
    // DO NOT ACCESS *this AFTER THIS POINT
}
```

### Pattern: Refcounted Pipe State

- **When**: Internal stream pipe operations with async continuations
- **How**: `Pipe::State` is `kj::Refcounted`; lambdas capture `kj::addRef(*state)`;
  `~Pipe()` sets `state->aborted = true`; continuations check before proceeding

```cpp
struct Pipe {
    struct State: public kj::Refcounted {
        bool aborted = false;
    };
    kj::Own<State> state;
    ~Pipe() noexcept(false) { state->aborted = true; }
};
```

### Pattern: Generation Counter

- **When**: `writeLoop()` -- detecting queue modification during async operation
- **How**: Capture generation before async op; assert unchanged after

```cpp
auto check = [expectedGeneration = queue.currentGeneration()]() {
    KJ_ASSERT(queue.currentGeneration() == expectedGeneration);
    return queue.front();
};
```

### Pattern: Promise Resolution Ordering

- **When**: State transitions paired with promise resolution
- **Rule**: Transition state BEFORE resolving promise. Continuations must see
  consistent state.

```cpp
void doClose(jsg::Lock& js) {
    state.transitionTo<StreamStates::Closed>();           // State changes NOW
    maybeResolvePromise(js, locked.getClosedFulfiller()); // Schedules microtask
}
```

### Pattern: V8Ref Buffer Ownership

- **When**: Async write operations that reference JS heap data
- **Why**: GC could collect the buffer while kj async write is in flight
- **How**: Store `jsg::V8Ref<v8::ArrayBuffer>` alongside raw pointer

```cpp
struct Write {
    jsg::V8Ref<v8::ArrayBuffer> ownBytes;  // Prevents GC
    kj::ArrayPtr<kj::byte> bytes;          // Raw pointer into ownBytes
};
```

## Cross-Request Model

Multiple requests share one isolate via green threads. When one request yields for
I/O, another may run. `SetPromiseCrossContextResolveCallback` defers promise reactions
to the correct request context.

Stream code interacts with this via:

- `ioContext.addFunctor()` -- binds continuations to correct `IoContext`
- `IoOwn<>` -- ensures objects accessed from correct context
- Promise context tagging -- all promises tagged with originating `IoContext`;
  reactions deferred if tag doesn't match current context
