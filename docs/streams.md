# Workers Streams Implementation Guide

This document walks through the streams implementation in workerd.

For terse reference material (classification tables, state machines, safety patterns), see
[src/workerd/api/streams/README.md](../src/workerd/api/streams/README.md).

For the file map and coding invariants, see
[src/workerd/api/streams/AGENTS.md](../src/workerd/api/streams/AGENTS.md).

## Overview

The Workers runtime has **two separate implementations** of the Streams API, both
exposed through the same `ReadableStream`, `WritableStream`, and `TransformStream`
JavaScript APIs.

**Internal streams** were the original implementation, built to serve workerd's own
needs: reading request bodies, writing response bodies, etc. They are a thin wrapper
around kj's asynchronous I/O primitives (`kj::AsyncInputStream`, `kj::AsyncOutputStream`)
and are only superficially related to the WHATWG Streams specification. Internal streams
are exclusively byte-oriented (they only handle `TypedArray` and `ArrayBuffer` data).

**Standard streams** are a separate, spec-compliant implementation of the WHATWG Streams
standard. They are driven entirely by JavaScript promises and user-provided callback
functions. Standard streams can be either byte-oriented or value-oriented (handling any
JavaScript value).

Both implementations coexist because removing the internal implementation would break
backwards compatibility. A compatibility flag system controls which behavior
`new TransformStream()` uses, and various APIs like `request.body` always return
internal streams while `new ReadableStream(...)` with user callbacks always creates
standard streams.

## Terminology

### Controllers

Every `ReadableStream` and `WritableStream` has an underlying **controller** that
provides the actual implementation. Internal and Standard streams each have their
own controller classes:

| Stream type      | Internal controller                | Standard controller          | File                        |
| ---------------- | ---------------------------------- | ---------------------------- | --------------------------- |
| `ReadableStream` | `ReadableStreamInternalController` | `ReadableStreamJsController` | `internal.h` / `standard.h` |
| `WritableStream` | `WritableStreamInternalController` | `WritableStreamJsController` | `internal.h` / `standard.h` |

### Byte-oriented vs. Value-oriented

A **byte-oriented** stream only handles `TypedArray` and `ArrayBuffer` data. A
**value-oriented** stream can handle any JavaScript value, including `undefined` and
`null`. A `TypedArray` or `ArrayBuffer` can pass through a value-oriented stream, but
it is treated as an opaque JavaScript value -- the stream does not interpret it as byte
data. Internal streams are always byte-oriented. Standard streams can be either.

Note that the `ReadableStream` and `WritableStream` APIs provide no way to inspect
which kind of data a stream handles.

### BYOB vs. Default Readers

A **Reader** consumes data from a `ReadableStream`. There are two kinds:

- A **BYOB Reader** (Bring Your Own Buffer) works only with byte-oriented streams.
  The caller provides a destination `TypedArray` that the stream fills with data.
- A **Default Reader** works with both byte-oriented and value-oriented streams.
  The stream provides data in whatever form it produces.

## How ReadableStream Works

Consider the basic usage pattern:

```js
const readableStream = getReadableSomehow();
const reader = readableStream.getReader();

const chunk = await reader.read();
console.log(chunk.value); // the data that was read
console.log(chunk.done); // true if the stream has completed
```

User code calls `read()` repeatedly until `chunk.done` is `true`. What happens
under the covers depends on whether this is an Internal or Standard stream.

### Standard ReadableStream

A Standard `ReadableStream` maintains two internal queues: a **queue of available data**
and a **queue of pending reads**. The controller uses four callback functions
(what the spec calls "algorithms"):

- **start** -- invoked immediately when the `ReadableStream` is created
- **pull** -- invoked to request more data from the source
- **cancel** -- invoked when the stream is explicitly canceled
- **size** -- determines the size of a chunk for backpressure calculations

When the stream is created, the start algorithm runs immediately. Once it completes,
the stream checks the **highwater mark** -- the maximum amount of data (calculated
using the size algorithm) that should be held in the data queue. If the current queue
size is below the highwater mark, the pull algorithm is called.

The pull algorithm pushes data into the ReadableStream (and yes, the irony of a "pull"
that pushes is not lost):

- If there are no pending reads, or the queue already has data, the new data goes
  into the queue.
- If there is a pending read, it is immediately fulfilled with the data. Any excess
  beyond what the read consumes goes into the queue.

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
                                          |
                                          v
                                        (done)
```

When `reader.read()` is called:

- If the queue has data, the read is fulfilled immediately. If this drops the queue
  size below the highwater mark, the pull algorithm is invoked.
- If the queue is empty, the read is added to the pending read queue. If the queue
  size is below the highwater mark, the pull algorithm is invoked.

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

**Important caveats:**

- The source can push data into the queue even when there are no active readers and
  even after backpressure has been signaled. This data accumulates in memory.
- It is possible to push more data than a reader can consume in one read. Excess
  stays in the queue.
- Once user code obtains a reference to the controller, it can enqueue data at any
  time, independent of the pull algorithm.

### Internal ReadableStream

Internal streams work very differently. The key difference: an Internal stream only
allows **a single pending read at a time**, has no internal data queue, and has no
pull algorithm.

```js
const readable = new ReadableStream(); // Standard stream
const reader = readable.getReader();
reader.read();
reader.read(); // Works fine -- queued as a pending read.

const readable = request.body; // Internal stream
const reader = readable.getReader();
reader.read();
reader.read(); // Fails with an error!
```

An Internal stream is backed by a `ReadableStreamSource`, which wraps a
`kj::AsyncInputStream`. Calling `read()` translates directly to a `tryRead()` on the
source, returning a `kj::Promise` for data. Only one read can be in flight at a time.

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

Data only flows through an Internal stream when there is an active reader, and each
read never exceeds the maximum byte count specified in the call.

### Tee

The `ReadableStream.tee()` method splits data flow into two separate `ReadableStream`
instances (branches).

In the WHATWG spec, `tee()` creates two branches that share a single reader on the
original stream (the "trunk"). When one branch pulls, the data fulfills that branch's
read and a **copy** is pushed into the other branch's queue. This means one branch
reading faster than the other causes unbounded memory growth in the slower branch.

Our implementation modifies this behavior: instead of copying data, branches hold
**refcounted references** (`kj::Rc<Entry>`) to shared data. Backpressure signaling
to the trunk is based on the branch with the most unconsumed data.

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

This doesn't completely prevent one branch from reading much slower than the other,
but it avoids the memory pileup that would otherwise occur -- as long as the
underlying source pays attention to backpressure signals.

## How WritableStream Works

### Standard WritableStream

A Standard `WritableStream` uses five algorithms:

- **start** -- prepares the stream to receive data
- **write** -- called for each chunk written
- **abort** -- called on abrupt termination
- **close** -- called after the last chunk
- **size** -- calculates chunk size for the highwater mark

```js
const writable = getWritableSomehow();
const writer = writable.getWriter();

await writer.write('chunk of data');
await writer.write('another chunk of data');
```

The write algorithm is asynchronous: every `write(data)` invokes it, and the returned
promise resolves when the write completes. This promise is the primary backpressure
mechanism. Backpressure is signaled in two ways:

- `writer.desiredSize` -- the amount of data that may be written before the queue is full
- `writer.ready` -- a promise that resolves when backpressure clears (replaced with a
  new promise each time backpressure is signaled)

Note that the highwater mark is advisory. A write algorithm should always expect to be
called regardless of the current highwater mark value.

### Internal WritableStream

An Internal `WritableStream` is backed by a `WritableStreamSink`, a thin wrapper around
`kj::AsyncOutputStream`. Every `write()` passes directly to the sink's `write()`. The
behavior from there depends on the specific sink implementation.

## TransformStream

A `TransformStream` connects a `ReadableStream` and a `WritableStream` -- data written
to the writable side can be read from the readable side, potentially transformed.

### IdentityTransformStream (Internal)

The original `TransformStream` was an identity transform: data passes through unmodified
from the writable side to the readable side. It uses a single class implementing both
`ReadableStreamSource` and `WritableStreamSink`.

This implementation is not spec-compliant. A compatibility flag changes `new TransformStream()`
to create a Standard TransformStream instead. The old behavior is available as
`new IdentityTransformStream()`.

```js
const { readable, writable } = new IdentityTransformStream();
const enc = new TextEncoder();

const writer = writable.getWriter();
const reader = readable.getReader();

// The write promise will not resolve until read() is called!
await Promise.all([writer.write(enc.encode('hello')), reader.read()]);
```

The `IdentityTransformStream` is a simple state machine allowing only one in-flight
read or write at a time. A `write()` promise won't resolve until a corresponding
`read()` occurs, and vice versa. It only supports byte data (`ArrayBuffer` / `TypedArray`).

### Standard TransformStream

A Standard `TransformStream` uses three algorithms:

- **start** -- initializes the transformation
- **transform** -- receives a chunk, modifies it, enqueues the result
- **flush** -- completes the transformation

```js
const { writable, readable } = new TransformStream({
  transform(chunk, controller) {
    controller.enqueue(`${chunk}!`.toUpperCase());
  },
});

const writer = writable.getWriter();
const reader = readable.getReader();

// The write promise does not wait for a read.
await writer.write('hello');
await reader.read(); // { value: 'HELLO!', done: false }
```

Both sides operate as full Standard streams with their own queues and backpressure.
Unlike the `IdentityTransformStream`, writes aren't blocked on reads (unless
backpressure signals the queue is full). Any JavaScript value can flow through.

## Piping

Piping sets up the flow of data from a `ReadableStream` to a `WritableStream`. The
destination writable determines how the pipe is implemented via its controller's
`tryPipeFrom()` method. There are four pipe loop variants depending on the stream types:

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

**kj-to-kj**: The most optimized case. kj pipes data directly from `AsyncInputStream` to
`AsyncOutputStream` outside the JavaScript isolate lock. No JavaScript runs at all.

**kj-to-JS / JS-to-kj**: These bridge kj and JavaScript promises. The entire flow must
occur under the isolate lock. Data is restricted to bytes because Standard streams
might be value-oriented and the API provides no way to inspect this.

**JS-to-JS**: Pure JavaScript promise chaining. Conceptually equivalent to:

```js
async function pipe(reader, writer) {
  for await (const chunk of reader) {
    await writer.write(chunk);
  }
}
```

### new Response(standardReadable)

Passing a Standard `ReadableStream` to APIs like `new Response()` is complicated because
these APIs were built for Internal streams and use kj async I/O internally.

To bridge this, Standard `ReadableStream`s can be consumed via the `ReadableStreamSource`
API (the same API Internal streams use). When `pumpTo()` is called on the adapter, it
acquires the isolate lock and runs a promise loop: read from the JS stream, write to the
kj output, repeat until the data is exhausted or an error occurs.

## The Complexity Budget

The streams implementation balances several sources of complexity:

- Two implementations of the same spec (Internal and Standard)
- Two data orientations (byte and value)
- Two memory heaps (JavaScript and kj)
- Two async models (JavaScript promises and kj promises)
- Strict backwards compatibility via feature flags

Internal stream data flow happens **outside** the isolate lock via kj's event loop.
Standard stream data flow happens **inside** the isolate lock via JavaScript promises.
When these two worlds interact (piping between types, passing standard streams to
internal APIs), bridging code must carefully manage both async models.

## Further Reading

- [AGENTS.md](../src/workerd/api/streams/AGENTS.md) -- file map, architecture summary, coding invariants
- [README.md](../src/workerd/api/streams/README.md) -- terse reference: classification, state machines, safety patterns
- [WHATWG Streams spec](https://streams.spec.whatwg.org/)
