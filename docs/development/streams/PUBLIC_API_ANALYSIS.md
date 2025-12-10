# Public API Implementation Analysis

This document analyzes the public-facing API classes for Web Streams: `TransformStream`, `ReadableStream`, `WritableStream`, and their associated reader/writer classes. These files (`transform.h/c++`, `readable.h/c++`, `writable.h/c++`) provide the JavaScript-visible interface and were not covered in the prior implementation analysis.

**Generated:** December 2025

---

## Table of Contents

1. [Overview](#1-overview)
2. [TransformStream Analysis](#2-transformstream-analysis)
3. [ReadableStream Analysis](#3-readablestream-analysis)
4. [WritableStream Analysis](#4-writablestream-analysis)
5. [Reader Implementation Analysis](#5-reader-implementation-analysis)
6. [Writer Implementation Analysis](#6-writer-implementation-analysis)
7. [RPC Serialization Analysis](#7-rpc-serialization-analysis)
8. [Potential Bugs](#8-potential-bugs)
9. [Code Smells](#9-code-smells)
10. [Improvement Opportunities](#10-improvement-opportunities)
11. [Summary](#11-summary)

---

## 1. Overview

### File Structure

| File | Purpose | Lines |
|------|---------|-------|
| `transform.h` | TransformStream class definition | 78 |
| `transform.c++` | TransformStream constructor logic | 117 |
| `readable.h` | ReadableStream, readers, queuing strategies | 484 |
| `readable.c++` | ReadableStream implementation | 762 |
| `writable.h` | WritableStream and writer | 196 |
| `writable.c++` | WritableStream implementation | 646 |

### Architectural Role

These files provide:
1. **JavaScript API surface** - The classes exposed to user scripts
2. **Reader/Writer pattern** - Lock acquisition for stream access
3. **Piping infrastructure** - `pipeThrough`, `pipeTo`
4. **Serialization** - RPC stream marshalling
5. **Async iteration** - `values()` and `[Symbol.asyncIterator]`

They delegate actual stream logic to the controller classes (`internal.c++` and `standard.c++`).

---

## 2. TransformStream Analysis

### 2.1 Class Design

**Location:** `transform.h:23-75`

```cpp
class TransformStream: public jsg::Object {
 public:
  explicit TransformStream(jsg::Ref<ReadableStream> readable, jsg::Ref<WritableStream> writable)
      : readable(kj::mv(readable)),
        writable(kj::mv(writable)) {}
```

The `TransformStream` class is a thin wrapper that holds references to a readable/writable pair. It doesn't contain transformation logic itself - that's delegated to `TransformStreamDefaultController`.

### 2.2 Constructor Analysis

**Location:** `transform.c++:25-114`

The constructor has two code paths based on a feature flag:

```cpp
if (FeatureFlags::get(js).getTransformStreamJavaScriptControllers()) {
  // Standard implementation path
} else {
  // Legacy IdentityTransformStream path
}
```

#### Standard Path (Feature Flag Enabled)

1. Creates a `TransformStreamDefaultController`
2. Creates a ReadableStream with lambdas that delegate to the controller
3. Creates a WritableStream with lambdas that delegate to the controller
4. Initializes the controller with references to both stream controllers

**Interesting Design Choice:**
```cpp
auto readableStrategy = kj::mv(maybeReadableStrategy)
                            .orDefault(StreamQueuingStrategy{
                              .highWaterMark = 0,
                            });
```

The default highWaterMark of 0 for the readable side means writes/reads are one-to-one (no buffering on readable side). Buffering happens on the writable side instead.

#### Legacy Path (Feature Flag Disabled)

Falls back to `IdentityTransformStream::constructor(js)` but logs a warning if any arguments were provided:

```cpp
if (maybeTransformer != kj::none || maybeWritableStrategy != kj::none ||
    maybeReadableStrategy != kj::none) {
  IoContext::current().logWarningOnce("To use the new TransformStream() constructor...");
}
```

### 2.3 Functor Wrapping Pattern

**Location:** `transform.c++:16-23`

```cpp
template <typename T>
jsg::Function<T> maybeAddFunctor(auto t) {
  if (IoContext::hasCurrent()) {
    return jsg::Function<T>(IoContext::current().addFunctor(kj::mv(t)));
  }
  return jsg::Function<T>(kj::mv(t));
}
```

This helper wraps lambdas with `IoContext::addFunctor` when an IoContext exists. This is necessary because:
1. The lambdas may outlive the current IoContext scope
2. `addFunctor` ensures proper lifetime management and execution context

**Potential Issue:** The pattern is used consistently but the `hasCurrent()` check suggests TransformStream can be created outside an IoContext (perhaps in tests or during module evaluation). The behavior differs between the two cases.

### 2.4 TransformStream Issues

#### Issue T1: Memory Tracking Incomplete

**Location:** `transform.h:63-66`

```cpp
void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("readable", readable);
  tracker.trackField("writable", writable);
}
```

The TransformStream's memory tracking visits the readable and writable streams, but the `TransformStreamDefaultController` is not directly tracked. The controller is held by the streams via closures, so it may be indirectly tracked, but this could lead to under-reporting of memory usage.

#### Issue T2: Controller Lifetime Coupling

The comment explains the design:
```cpp
// Persistent references to the TransformStreamDefaultController are held by both
// the readable and writable sides. The actual TransformStream object can be dropped
// and allowed to be garbage collected.
```

This means the controller outlives the TransformStream wrapper. While intentional, it creates a non-obvious ownership model where the controller is kept alive by closures in both streams.

---

## 3. ReadableStream Analysis

### 3.1 Class Structure

**Location:** `readable.h:190-411`

```cpp
class ReadableStream: public jsg::Object {
 public:
  explicit ReadableStream(IoContext& ioContext, kj::Own<ReadableStreamSource> source);
  explicit ReadableStream(kj::Own<ReadableStreamController> controller);
```

Two constructors:
1. From a `ReadableStreamSource` (creates internal controller)
2. From a pre-built controller (used by JS constructor)

### 3.2 Async Iterator Implementation

**Location:** `readable.h:193-207`, `readable.c++:439-468`

```cpp
struct AsyncIteratorState {
  kj::Maybe<IoContext&> ioContext;
  jsg::Ref<ReadableStreamDefaultReader> reader;
  bool preventCancel;
};
```

The async iterator acquires a reader and delegates to it:

```cpp
jsg::Promise<kj::Maybe<jsg::Value>> ReadableStream::nextFunction(
    jsg::Lock& js, AsyncIteratorState& state) {
  return state.reader->read(js).then(
      js, [reader = state.reader.addRef()](jsg::Lock& js, ReadResult result) mutable {
    if (result.done) {
      reader->releaseLock(js);
      return js.resolvedPromise(kj::Maybe<jsg::Value>(kj::none));
    }
    return js.resolvedPromise<kj::Maybe<jsg::Value>>(kj::mv(result.value));
  });
}
```

**Observation:** The reader reference is captured in both the state AND the lambda. The lambda capture ensures the reader survives until the promise resolves.

### 3.3 pipeThrough Implementation

**Location:** `readable.c++:377-398`

```cpp
jsg::Ref<ReadableStream> ReadableStream::pipeThrough(
    jsg::Lock& js, Transform transform, jsg::Optional<PipeToOptions> maybeOptions) {
  // ...
  auto options = kj::mv(maybeOptions).orDefault({});
  options.pipeThrough = true;  // Mark as pipeThrough for different error handling
  controller.pipeTo(js, destination, kj::mv(options))
      .then(js,
          JSG_VISITABLE_LAMBDA(
              (self = JSG_THIS), (self), (jsg::Lock& js) { return js.resolvedPromise(); }),
          JSG_VISITABLE_LAMBDA((self = JSG_THIS), (self),
              (jsg::Lock& js, auto&& exception) {
                return js.rejectedPromise<void>(kj::mv(exception));
              }))
      .markAsHandled(js);
  return kj::mv(transform.readable);
}
```

**Important Pattern:** The `options.pipeThrough = true` flag changes error handling behavior. In `pipeThrough`, errors are not propagated to the returned promise (it's marked as handled). This matches spec behavior where `pipeThrough` returns the readable side immediately while piping happens in the background.

### 3.4 detach Method

**Location:** `readable.c++:470-475`

```cpp
jsg::Ref<ReadableStream> ReadableStream::detach(jsg::Lock& js, bool ignoreDisturbed) {
  JSG_REQUIRE(
      !isDisturbed() || ignoreDisturbed, TypeError, "The ReadableStream has already been read.");
  JSG_REQUIRE(!isLocked(), TypeError, "The ReadableStream has been locked to a reader.");
  return js.alloc<ReadableStream>(getController().detach(js, ignoreDisturbed));
}
```

This implements the "create a proxy" algorithm from the spec more efficiently than creating a TransformStream and piping through it. The comment explains:

```cpp
// In that algorithm, it says to create a proxy of a stream by creating a new TransformStream
// and piping the original through it... That is quite inefficient so instead, we create a new
// ReadableStream that will take over ownership of the internal state of this one
```

### 3.5 EOF Signaling

**Location:** `readable.h:386-394`, `readable.c++:323-332`

```cpp
kj::Maybe<jsg::PromiseResolverPair<void>> eofResolverPair;

jsg::Promise<void> ReadableStream::onEof(jsg::Lock& js) {
  eofResolverPair = js.newPromiseAndResolver<void>();
  return kj::mv(KJ_ASSERT_NONNULL(eofResolverPair).promise);
}

void ReadableStream::signalEof(jsg::Lock& js) {
  KJ_IF_SOME(pair, eofResolverPair) {
    pair.resolver.resolve(js);
  }
}
```

This is used for TCP sockets that need explicit EOF notification. The comment notes:
```cpp
// This signal is required for TCP sockets.
```

### 3.6 ReadableStream Issues

#### Issue R1: tee() Error Message Typo

**Location:** `readable.c++:417-418`

```cpp
kj::Array<jsg::Ref<ReadableStream>> ReadableStream::tee(jsg::Lock& js) {
  JSG_REQUIRE(!isLocked(), TypeError, "This ReadableStream is currently locked to a reader,");
```

The error message ends with a comma instead of a period. Minor but inconsistent with other error messages.

#### Issue R2: onEof Can Only Be Called Once

```cpp
jsg::Promise<void> ReadableStream::onEof(jsg::Lock& js) {
  eofResolverPair = js.newPromiseAndResolver<void>();  // Overwrites any existing!
  return kj::mv(KJ_ASSERT_NONNULL(eofResolverPair).promise);
}
```

If `onEof` is called twice, the first promise will never resolve. The comment says "This method should only be called once" but there's no enforcement.

#### Issue R3: Inconsistent Lock Checking

Compare these two methods:

```cpp
// Uses JSG_REQUIRE (throws synchronously)
kj::Array<jsg::Ref<ReadableStream>> ReadableStream::tee(jsg::Lock& js) {
  JSG_REQUIRE(!isLocked(), TypeError, "...");

// Returns rejected promise
jsg::Promise<void> ReadableStream::cancel(...) {
  if (isLocked()) {
    return js.rejectedPromise<void>(js.v8TypeError("..."));
  }
```

The inconsistency is likely intentional (tee is synchronous, cancel is async), but it creates different error handling patterns for callers.

---

## 4. WritableStream Analysis

### 4.1 Class Structure

**Location:** `writable.h:104-193`

```cpp
class WritableStream: public jsg::Object {
 public:
  explicit WritableStream(IoContext& ioContext,
      kj::Own<WritableStreamSink> sink,
      kj::Maybe<kj::Own<ByteStreamObserver>> observer,
      kj::Maybe<uint64_t> maybeHighWaterMark = kj::none,
      kj::Maybe<jsg::Promise<void>> maybeClosureWaitable = kj::none);
```

Notable parameters:
- `observer`: For metrics/telemetry on byte streams
- `maybeHighWaterMark`: Backpressure threshold
- `maybeClosureWaitable`: Promise that resolves when underlying closure completes

### 4.2 WeakRef Pattern

**Location:** `writable.h:182-188`

```cpp
kj::Own<WeakRef<WritableStream>> weakRef =
    kj::refcounted<WeakRef<WritableStream>>(kj::Badge<WritableStream>(), *this);

kj::Own<WeakRef<WritableStream>> addWeakRef() {
  return weakRef->addRef();
}
```

The WritableStream maintains a weak reference to itself, used by the RPC serialization layer. The destructor invalidates it:

```cpp
~WritableStream() noexcept(false) {
  weakRef->invalidate();
}
```

### 4.3 flush() Method

**Location:** `writable.c++:283-289`

```cpp
jsg::Promise<void> WritableStream::flush(jsg::Lock& js) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently locked to a writer."_kj));
  }
  return getController().flush(js);
}
```

This is a non-standard extension that allows flushing buffered data without closing.

### 4.4 removeSink Deprecation

**Location:** `writable.h:126`

```cpp
virtual KJ_DEPRECATED("Use detach() instead") kj::Own<WritableStreamSink> removeSink(
    jsg::Lock& js);
```

The deprecation suggests an ongoing migration from `removeSink` to `detach`. The RPC serialization code still uses `removeSink`:

```cpp
KJ_IF_SOME(sink, getController().removeSink(js)) {
  // ...
}
```

### 4.5 WritableStream Issues

#### Issue W1: Virtual Methods on Non-Base Class

```cpp
virtual KJ_DEPRECATED("Use detach() instead") kj::Own<WritableStreamSink> removeSink(...);
virtual void detach(jsg::Lock& js);
```

These are marked `virtual` but `WritableStream` doesn't appear to have subclasses. This may be legacy or forward-looking design, but adds vtable overhead.

#### Issue W2: inspectState Inconsistency

**Location:** `writable.c++:199-209`

```cpp
jsg::JsString WritableStream::inspectState(jsg::Lock& js) {
  if (controller->isErrored()) {
    return js.strIntern("errored");
  } else if (controller->isErroring(js) != kj::none) {
    return js.strIntern("erroring");
  } else if (controller->isClosedOrClosing()) {
    return js.strIntern("closed");  // Should be "closed" OR "closing"?
  } else {
    return js.strIntern("writable");
  }
}
```

The `isClosedOrClosing()` case returns `"closed"` but doesn't distinguish between closed and closing states. Compare to the more granular state representation in the internal controller.

---

## 5. Reader Implementation Analysis

### 5.1 ReaderImpl Shared Implementation

**Location:** `readable.h:15-62`, `readable.c++:17-168`

Both `ReadableStreamDefaultReader` and `ReadableStreamBYOBReader` delegate to a shared `ReaderImpl`:

```cpp
class ReaderImpl final {
  // ...
  kj::OneOf<Initial, Attached, StreamStates::Closed, Released> state = Initial();
  kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<void>>> closedPromise;
};
```

This is good design - it avoids code duplication between the two reader types.

### 5.2 State Machine

The reader has four states:

| State | Description |
|-------|-------------|
| Initial | Created but not attached to a stream |
| Attached | Actively reading from a stream |
| Closed | Stream was closed while reader held |
| Released | Reader released lock on stream |

### 5.3 BYOB Reader Read Validation

**Location:** `readable.c++:91-118`

```cpp
jsg::Promise<ReadResult> ReaderImpl::read(
    jsg::Lock& js, kj::Maybe<ReadableStreamController::ByobOptions> byobOptions) {
  // ...
  KJ_IF_SOME(options, byobOptions) {
    size_t atLeast = options.atLeast.orDefault(1);

    if (options.byteLength == 0) {
      return js.rejectedPromise<ReadResult>(
          js.v8TypeError("You must call read() on a \"byob\" reader with a positive-sized "
                         "TypedArray object."_kj));
    }
    if (atLeast == 0) {
      return js.rejectedPromise<ReadResult>(js.v8TypeError(
          kj::str("Requested invalid minimum number of bytes to read (", atLeast, ").")));
    }
    if (atLeast > options.byteLength) {
      return js.rejectedPromise<ReadResult>(js.v8TypeError(kj::str("Minimum bytes to read (",
          atLeast, ") exceeds size of buffer (", options.byteLength, ").")));
    }
```

Good defensive validation before attempting the read.

### 5.4 readAtLeast Non-Standard Extension

**Location:** `readable.h:143-145`, `readable.c++:272-282`

```cpp
jsg::Promise<ReadResult> ReadableStreamBYOBReader::readAtLeast(
    jsg::Lock& js, int minBytes, v8::Local<v8::ArrayBufferView> byobBuffer) {
  auto options = ReadableStreamController::ByobOptions{
    // ...
    .atLeast = minBytes,
    .detachBuffer = true,  // Always detaches
  };
  return impl.read(js, kj::mv(options));
}
```

This is a non-standard extension (documented in the header). Unlike the standard `read()` with options, this always detaches the buffer.

### 5.5 Reader Issues

#### Issue RE1: TODO Comment for Spec Compliance

**Location:** `readable.c++:134-136`

```cpp
void ReaderImpl::releaseLock(jsg::Lock& js) {
  // TODO(soon): Releasing the lock should cancel any pending reads. This is a recent
  // modification to the spec that we have not yet implemented.
```

This is a known spec non-compliance. The spec was updated to require canceling pending reads on release, but this hasn't been implemented.

#### Issue RE2: Cancel After Release Still Possible

**Location:** `readable.c++:66-68`

When `cancel` is called on a Released reader, it returns a rejected promise:

```cpp
KJ_CASE_ONEOF(r, Released) {
  return js.rejectedPromise<void>(
      js.v8TypeError("This ReadableStream reader has been released."_kj));
}
```

But when called on a Closed reader, it returns a resolved promise:

```cpp
KJ_CASE_ONEOF(c, StreamStates::Closed) {
  return js.resolvedPromise();
}
```

This asymmetry might surprise callers.

#### Issue RE3: readAtLeast Uses int Instead of size_t

**Location:** `readable.h:143`

```cpp
jsg::Promise<ReadResult> readAtLeast(jsg::Lock& js,
                                      int minBytes,  // Should be size_t?
                                      v8::Local<v8::ArrayBufferView> byobBuffer);
```

Using `int` allows negative values which are nonsensical. Should be `size_t` or `uint32_t`.

---

## 6. Writer Implementation Analysis

### 6.1 WritableStreamDefaultWriter

**Location:** `writable.h:13-102`, `writable.c++:13-220`

Similar to readers, but with additional `ready` promise for backpressure:

```cpp
kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<void>>> closedPromise;
kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<void>>> readyPromise;
kj::Maybe<jsg::Promise<void>> readyPromisePending;
```

### 6.2 Ready Promise Management

**Location:** `writable.c++:174-178`

```cpp
void WritableStreamDefaultWriter::replaceReadyPromise(
    jsg::Lock& js, jsg::Promise<void> readyPromise) {
  this->readyPromisePending = kj::mv(readyPromise);
  this->readyPromise = KJ_ASSERT_NONNULL(this->readyPromisePending).whenResolved(js);
}
```

The `ready` promise is replaced when backpressure state changes. The controller calls `replaceReadyPromise` when the writer should wait.

### 6.3 Writer Issues

#### Issue WR1: TODO Comment for Spec Compliance

**Location:** `writable.c++:146-147`

```cpp
void WritableStreamDefaultWriter::releaseLock(jsg::Lock& js) {
  // TODO(soon): Releasing the lock should cancel any pending writes.
```

Same issue as readers - pending operations should be canceled on release per recent spec updates.

#### Issue WR2: No BYOB Writer

The streams spec doesn't define a BYOB writer, but some use cases (zero-copy writes) could benefit from one. This is more of a spec limitation than an implementation issue.

---

## 7. RPC Serialization Analysis

### 7.1 ReadableStream Serialization

**Location:** `readable.c++:657-700`

The serialization creates a Cap'n Proto ByteStream capability:

```cpp
void ReadableStream::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  // ...
  auto streamCap = externalHandler->writeStream(
      [encoding, expectedLength](rpc::JsValue::External::Builder builder) mutable {
    auto rs = builder.initReadableStream();
    rs.setEncoding(encoding);
    // ...
  });

  kj::Own<capnp::ExplicitEndOutputStream> kjStream =
      ioctx.getByteStreamFactory().capnpToKjExplicitEnd(
          kj::mv(streamCap).castAs<capnp::ByteStream>());

  auto sink = newSystemStream(kj::mv(kjStream), encoding, ioctx);

  ioctx.addTask(
      ioctx.waitForDeferredProxy(pumpTo(js, kj::mv(sink), true)).catch_([](kj::Exception&& e) {
    // Errors silently swallowed
  }));
}
```

### 7.2 ExplicitEndPipeAdapter Pattern

**Location:** `readable.c++:522-603`

To bridge Cap'n Proto's explicit end semantics with KJ pipes:

```cpp
class ExplicitEndOutputPipeAdapter final: public capnp::ExplicitEndOutputStream {
  kj::Promise<void> end() override {
    ended->getWrapped() = true;  // Signal end was called
    inner = kj::none;
    return kj::READY_NOW;
  }
};

class ExplicitEndInputPipeAdapter final: public kj::AsyncInputStream {
  kj::Promise<size_t> tryRead(...) {
    // ...
    if (result < minBytes) {
      if (!ended->getWrapped()) {
        JSG_FAIL_REQUIRE(Error, "ReadableStream received over RPC disconnected prematurely.");
      }
    }
  }
};
```

The `ended` flag communicates between the output and input sides of the pipe to detect premature disconnection.

### 7.3 WritableStream RPC Adapters

**Location:** `writable.c++:311-549`

Two adapters exist:

1. **WritableStreamRpcAdapter** - For streams with native `WritableStreamSink`
2. **WritableStreamJsRpcAdapter** - For JS-backed streams without a sink

The JS adapter is more complex because it must acquire the isolate lock for each write:

```cpp
kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
  // ...
  return canceler.wrap(context.run([this, buffer](Worker::Lock& lock) mutable {
    auto& writer = getInner();
    auto source = KJ_ASSERT_NONNULL(jsg::BufferSource::tryAlloc(lock, buffer.size()));
    source.asArrayPtr().copyFrom(buffer);  // Must copy!
    return context.awaitJs(lock, writer.write(lock, source.getHandle(lock)));
  }));
}
```

### 7.4 RPC Issues

#### Issue RPC1: Error Swallowing

**Location:** `readable.c++:695-699`

```cpp
ioctx.addTask(
    ioctx.waitForDeferredProxy(pumpTo(js, kj::mv(sink), true)).catch_([](kj::Exception&& e) {
  // Errors in pumpTo() are automatically propagated to the source and destination. We don't
  // want to throw them from here since it'll cause an uncaught exception to be reported
}));
```

While the comment explains the rationale, silently swallowing exceptions can hide bugs.

#### Issue RPC2: WritableStreamJsRpcAdapter Buffer Copy

**Location:** `writable.c++:458-463`, `writable.c++:475-492`

```cpp
// Sadly, we have to allocate and copy here. Our received set of buffers are only
// guaranteed to live until the returned promise is resolved, but the application code
// may hold onto the ArrayBuffer for longer.
auto source = KJ_ASSERT_NONNULL(jsg::BufferSource::tryAlloc(lock, amount));
```

Every write through a JS-backed RPC stream requires a buffer copy. This is unavoidable given the lifetime constraints but is a performance concern for high-throughput scenarios.

#### Issue RPC3: NoDeferredProxyReadableStream Workaround

**Location:** `readable.c++:611-653`

```cpp
// Wrapper around ReadableStreamSource that prevents deferred proxying. We need this for RPC
// streams because although they are "system streams", they become disconnected when the IoContext
// is destroyed, due to the JsRpcCustomEvent being canceled.
class NoDeferredProxyReadableStream final: public ReadableStreamSource {
```

This is a workaround for a lifetime issue. The TODO suggests there should be a better solution:

```cpp
// TODO(someday): Devise a better way for RPC streams to extend the lifetime of the RPC session
//   beyond the destruction of the IoContext, if it is being used for deferred proxying.
```

#### Issue RPC4: Typo in Error Message

**Location:** `writable.c++:373-376`, `writable.c++:544-548`

```cpp
return JSG_KJ_EXCEPTION(DISCONNECTED, Error,
    "WritableStream received over RPC was disconnected because the remote execution context "
    "has endeded.");  // "endeded" should be "ended"
```

The typo "endeded" appears twice.

#### Issue RPC5: whenWriteDisconnected Returns NEVER_DONE

**Location:** `writable.c++:351-354`, `writable.c++:500-514`

```cpp
kj::Promise<void> whenWriteDisconnected() override {
  // TODO(someday): WritableStreamSink doesn't give us a way to implement this.
  return kj::NEVER_DONE;
}
```

Both RPC adapters return `NEVER_DONE` for `whenWriteDisconnected()`. This means Cap'n Proto can't detect disconnection and do path shortening or early cleanup.

---

## 8. Potential Bugs

### Bug 1: readAtLeast Allows Negative minBytes

**Location:** `readable.h:143`

```cpp
jsg::Promise<ReadResult> readAtLeast(jsg::Lock& js,
                                      int minBytes,  // Can be negative!
                                      v8::Local<v8::ArrayBufferView> byobBuffer);
```

**Impact:** Low - would likely cause assertion failure or nonsensical read behavior.

**Fix:** Use `uint32_t` or add validation.

### Bug 2: onEof Overwrites Previous Promise

**Location:** `readable.c++:323-326`

```cpp
jsg::Promise<void> ReadableStream::onEof(jsg::Lock& js) {
  eofResolverPair = js.newPromiseAndResolver<void>();  // Silently overwrites
  return kj::mv(KJ_ASSERT_NONNULL(eofResolverPair).promise);
}
```

**Impact:** Medium - if called twice, first caller's promise never resolves.

**Fix:** Either assert that `eofResolverPair` is `kj::none`, or return the existing promise.

### Bug 3: Typo in Error Message

**Location:** `writable.c++:375`, `writable.c++:546`

```cpp
"has endeded."  // Should be "ended"
```

**Impact:** Low - cosmetic only.

### Bug 4: Missing Spec Compliance for releaseLock

**Location:** `readable.c++:134-136`, `writable.c++:146-147`

Both readers and writers have TODO comments indicating pending reads/writes should be canceled on release, but this isn't implemented.

**Impact:** Medium - can cause spec-non-compliant behavior where pending operations complete after release.

---

## 9. Code Smells

### Smell 1: Inconsistent Error Handling Patterns

Three different patterns are used:

```cpp
// Pattern 1: JSG_REQUIRE (throws synchronously)
JSG_REQUIRE(!isLocked(), TypeError, "...");

// Pattern 2: Return rejected promise
if (isLocked()) {
  return js.rejectedPromise<void>(js.v8TypeError("..."));
}

// Pattern 3: JSG_FAIL_REQUIRE
JSG_FAIL_REQUIRE(TypeError, "message");
```

**Recommendation:** Document when to use each pattern. Generally:
- Synchronous functions: JSG_REQUIRE
- Async functions: Return rejected promise
- Never-return functions: JSG_FAIL_REQUIRE

### Smell 2: Magic Numbers

**Location:** `readable.c++:372-374`

```cpp
return js.alloc<ReadableStreamAsyncIterator>(AsyncIteratorState{.ioContext = ioContext,
  .reader = ReadableStreamDefaultReader::constructor(js, JSG_THIS),
  .preventCancel = options.orDefault(defaultOptions).preventCancel.orDefault(false)});
```

The `false` default is repeated in the ValuesOptions struct definition. Should reference the struct.

### Smell 3: Duplicate Error Strings

Same error messages appear multiple times:

```cpp
"This ReadableStream is currently locked to a reader."  // 3 occurrences
"This WritableStream is currently locked to a writer."  // 3 occurrences
"This WritableStream has been closed."                  // 2 occurrences
```

**Recommendation:** Define as `constexpr` strings in `common.h`.

### Smell 4: Mixed Promise Patterns in pipeThrough

**Location:** `readable.c++:388-396`

```cpp
controller.pipeTo(js, destination, kj::mv(options))
    .then(js,
        JSG_VISITABLE_LAMBDA(
            (self = JSG_THIS), (self), (jsg::Lock& js) { return js.resolvedPromise(); }),
        JSG_VISITABLE_LAMBDA((self = JSG_THIS), (self),
            (jsg::Lock& js, auto&& exception) {
              return js.rejectedPromise<void>(kj::mv(exception));
            }))
    .markAsHandled(js);
```

The success handler returns `js.resolvedPromise()` but the value is discarded. The error handler re-wraps the exception. This is overly complex - could use simpler callbacks.

### Smell 5: Virtual Methods Without Inheritance

**Location:** `writable.h:126-128`

```cpp
virtual KJ_DEPRECATED("Use detach() instead") kj::Own<WritableStreamSink> removeSink(...);
virtual void detach(jsg::Lock& js);
```

These virtual methods don't appear to be overridden anywhere. Consider removing `virtual` or documenting the intended use.

---

## 10. Improvement Opportunities

### Improvement 1: Consolidate Error Messages

Create a central location for stream error messages:

```cpp
// In common.h
namespace StreamErrors {
  constexpr auto READABLE_LOCKED = "This ReadableStream is currently locked to a reader."_kj;
  constexpr auto WRITABLE_LOCKED = "This WritableStream is currently locked to a writer."_kj;
  constexpr auto READER_RELEASED = "This ReadableStream reader has been released."_kj;
  constexpr auto WRITER_RELEASED = "This WritableStream writer has been released."_kj;
}
```

### Improvement 2: Type-Safe readAtLeast

Change signature to prevent negative values:

```cpp
jsg::Promise<ReadResult> readAtLeast(jsg::Lock& js,
                                      uint32_t minBytes,  // or size_t
                                      v8::Local<v8::ArrayBufferView> byobBuffer);
```

### Improvement 3: Protect onEof from Double-Call

```cpp
jsg::Promise<void> ReadableStream::onEof(jsg::Lock& js) {
  KJ_ASSERT(eofResolverPair == kj::none, "onEof() can only be called once");
  eofResolverPair = js.newPromiseAndResolver<void>();
  return kj::mv(KJ_ASSERT_NONNULL(eofResolverPair).promise);
}
```

### Improvement 4: Implement releaseLock Spec Compliance

Add pending operation cancellation to both readers and writers when `releaseLock` is called. This is marked as TODO(soon) in the code.

### Improvement 5: Add whenWriteDisconnected Support

Implement proper `whenWriteDisconnected()` for RPC adapters by tracking the writer's closed promise. This would enable Cap'n Proto to do path shortening and early cleanup.

### Improvement 6: Memory Tracking for TransformStream Controller

Track the `TransformStreamDefaultController` in memory info:

```cpp
void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("readable", readable);
  tracker.trackField("writable", writable);
  // Note: Controller is tracked indirectly through readable/writable closures
}
```

Or refactor to hold a direct reference to the controller for more accurate tracking.

### Improvement 7: Document IoContext-Less Behavior

The `maybeAddFunctor` pattern suggests TransformStream can be created without an IoContext. Document when this happens and the behavioral differences.

---

## 11. Summary

### Quality Assessment

| Aspect | Score | Notes |
|--------|-------|-------|
| API Design | 4/5 | Clean, spec-aligned interface |
| Code Quality | 3/5 | Some duplication, inconsistent patterns |
| Error Handling | 3/5 | Inconsistent patterns, some swallowed errors |
| Spec Compliance | 3/5 | Known TODOs for releaseLock behavior |
| Performance | 3/5 | RPC path has unavoidable copies |
| Memory Safety | 4/5 | Good GC integration, proper ref counting |
| Documentation | 4/5 | Good comments explaining non-obvious behavior |

### Critical Issues

1. **releaseLock doesn't cancel pending operations** - Spec non-compliance
2. **onEof can only be called once** - No enforcement, silent failure

### High-Priority Fixes

1. Fix "endeded" typo in error messages
2. Use unsigned type for `readAtLeast` parameter
3. Add assertion to prevent double `onEof` call

### Medium-Priority Improvements

1. Consolidate error message strings
2. Implement releaseLock cancellation
3. Document IoContext-less behavior

### Architecture Notes

The public API classes are well-designed thin wrappers that delegate to controller implementations. The ReaderImpl/WriterImpl shared implementation pattern is effective at reducing duplication. The RPC serialization layer is complex but necessarily so due to lifetime and threading constraints.

---

## Document Information

| Property | Value |
|----------|-------|
| Files Analyzed | transform.h/c++, readable.h/c++, writable.h/c++ |
| Total Lines | ~2,283 |
| Focus | Public API, readers/writers, RPC serialization |
| Complements | STREAMS_IMPLEMENTATION_REPORT.md (controllers), CODE_PATTERNS_ANALYSIS.md |
