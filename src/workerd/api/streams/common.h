// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/io/io-context.h>
#include "../basics.h"

#if _MSC_VER
typedef long long ssize_t;
#endif

namespace workerd::api {

class ReadableStream;
class ReadableStreamController;
class ReadableStreamSource;
class ReadableStreamDefaultController;
class ReadableByteStreamController;

class WritableStream;
class WritableStreamController;
class WritableStreamSink;
class WritableStreamDefaultController;

class TransformStreamDefaultController;

enum class StreamEncoding {
  IDENTITY,
  GZIP,
  BROTLI
};

struct ReadResult {
  jsg::Optional<jsg::Value> value;
  bool done;

  JSG_STRUCT(value, done);
  JSG_STRUCT_TS_OVERRIDE(type ReadableStreamReadResult<R = any> =
    | { done: false, value: R; }
    | { done: true; value?: undefined; }
  );

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(value);
  }
};

struct StreamQueuingStrategy {
  using SizeAlgorithm = uint64_t(v8::Local<v8::Value>);

  jsg::Optional<uint64_t> highWaterMark;
  jsg::Optional<jsg::Function<SizeAlgorithm>> size;

  JSG_STRUCT(highWaterMark, size);
  JSG_STRUCT_TS_OVERRIDE(QueuingStrategy<T = any> {
    size?: (chunk: T) => number | bigint;
  });
};

struct UnderlyingSource {
  using Controller = kj::OneOf<jsg::Ref<ReadableStreamDefaultController>,
                               jsg::Ref<ReadableByteStreamController>>;
  using StartAlgorithm = jsg::Promise<void>(Controller);
  using PullAlgorithm = jsg::Promise<void>(Controller);
  using CancelAlgorithm = jsg::Promise<void>(v8::Local<v8::Value> reason);

  // The autoAllocateChunkSize mechanism allows byte streams to operate as if a BYOB
  // reader is being used even if it is just a default reader. Support is optional
  // per the streams spec but our implementation will always enable it. Specifically,
  // if user code does not provide an explicit autoAllocateChunkSize, we'll assume
  // this default.
  static constexpr int DEFAULT_AUTO_ALLOCATE_CHUNK_SIZE = 4096;

  // Per the spec, the type property for the UnderlyingSource should be either
  // undefined, the empty string, or "bytes". When undefined, the empty string is
  // used as the default. When type is the empty string, the stream is considered
  // to be value-oriented rather than byte-oriented.
  jsg::Optional<kj::String> type;

  // Used only when type is equal to "bytes", the autoAllocateChunkSize defines
  // the size of automatically allocated buffer that is created when a default
  // mode read is performed on a byte-oriented ReadableStream that supports
  // BYOB reads. The stream standard makes this optional to support and defines
  // no default value. We've chosen to use a default value of 4096. If given,
  // the value must be greater than zero.
  jsg::Optional<int> autoAllocateChunkSize;

  jsg::Optional<jsg::Function<StartAlgorithm>> start;
  jsg::Optional<jsg::Function<PullAlgorithm>> pull;
  jsg::Optional<jsg::Function<CancelAlgorithm>> cancel;

  JSG_STRUCT(type, autoAllocateChunkSize, start, pull, cancel);
  JSG_STRUCT_TS_DEFINE(interface UnderlyingByteSource {
    type: "bytes";
    autoAllocateChunkSize?: number;
    start?: (controller: ReadableByteStreamController) => void | Promise<void>;
    pull?: (controller: ReadableByteStreamController) => void | Promise<void>;
    cancel?: (reason: any) => void | Promise<void>;
  });
  JSG_STRUCT_TS_OVERRIDE(<R = any> {
    type?: "" | undefined;
    autoAllocateChunkSize: never;
    start?: (controller: ReadableStreamDefaultController<R>) => void | Promise<void>;
    pull?: (controller: ReadableStreamDefaultController<R>) => void | Promise<void>;
    cancel?: (reason: any) => void | Promise<void>;
  });
};

struct UnderlyingSink {
  using Controller = jsg::Ref<WritableStreamDefaultController>;
  using StartAlgorithm = jsg::Promise<void>(Controller);
  using WriteAlgorithm = jsg::Promise<void>(v8::Local<v8::Value>, Controller);
  using AbortAlgorithm = jsg::Promise<void>(v8::Local<v8::Value> reason);
  using CloseAlgorithm = jsg::Promise<void>();

  // Per the spec, the type property for the UnderlyingSink should always be either
  // undefined or the empty string. Any other value will trigger a TypeError.
  jsg::Optional<kj::String> type;

  jsg::Optional<jsg::Function<StartAlgorithm>> start;
  jsg::Optional<jsg::Function<WriteAlgorithm>> write;
  jsg::Optional<jsg::Function<AbortAlgorithm>> abort;
  jsg::Optional<jsg::Function<CloseAlgorithm>> close;

  JSG_STRUCT(type, start, write, abort, close);

  // TODO(cleanp): Get rid of this override and parse the type directly in param-extractor.rs
  JSG_STRUCT_TS_OVERRIDE(<W = any> {
    write?: (chunk: W, controller: WritableStreamDefaultController) => void | Promise<void>;
    start?: (controller: WritableStreamDefaultController) => void | Promise<void>;
    abort?: (reason: any) => void | Promise<void>;
    close?: () => void | Promise<void>;
  });
};

struct Transformer {
  using Controller = jsg::Ref<TransformStreamDefaultController>;
  using StartAlgorithm = jsg::Promise<void>(Controller);
  using TransformAlgorithm = jsg::Promise<void>(v8::Local<v8::Value>, Controller);
  using FlushAlgorithm = jsg::Promise<void>(Controller);

  jsg::Optional<kj::String> readableType;
  jsg::Optional<kj::String> writableType;

  jsg::Optional<jsg::Function<StartAlgorithm>> start;
  jsg::Optional<jsg::Function<TransformAlgorithm>> transform;
  jsg::Optional<jsg::Function<FlushAlgorithm>> flush;

  JSG_STRUCT(readableType, writableType, start, transform, flush);
  JSG_STRUCT_TS_OVERRIDE(<I = any, O = any> {
    start?: (controller: TransformStreamDefaultController<O>) => void | Promise<void>;
    transform?: (chunk: I, controller: TransformStreamDefaultController<O>) => void | Promise<void>;
    flush?: (controller: TransformStreamDefaultController<O>) => void | Promise<void>;
  });
};

// ReadableStreamSource and WritableStreamSink
//
// These are implementation interfaces for ReadableStream and WritableStream. If you just need to
// use a ReadableStream or WritableStream, you can safely skip reading this. If you need to
// implement a new kind of stream, read on.

// In the original Workers streams implementation, a ReadableStream would have a
// ReadableStreamSource backing it. Likewise, a WritableStream would have a WritableStreamSink.
// The ReadableStreamSource and WritableStreamSink are kj heap objects that provide a thin
// wrapper on internal native stream sources originating from within the Workers runtime.
//
// With implementation of full streams standard support, we introduce the new abstraction APIs
// ReadableStreamController and WritableStreamController, which will provide the underlying
// implementation for both ReadableStream and WritableStream, respectively.
//
// When creating a new kind of *internal* ReadableStream, where the data is originating internally
// from a kj stream, you will still implement the ReadableStreamSource API, just as before.
// Likewise, when creating a new kind of *internal* WritableStream, where the data destination is
// a kj stream, you will implement the WritableStreamSink API.

class WritableStreamSink {
public:
  virtual kj::Promise<void> write(const void* buffer, size_t size)
      KJ_WARN_UNUSED_RESULT = 0;
  virtual kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces)
      KJ_WARN_UNUSED_RESULT = 0;

  virtual kj::Promise<void> end() KJ_WARN_UNUSED_RESULT = 0;
  // Must call to flush and finish the stream.

  virtual kj::Maybe<kj::Promise<DeferredProxy<void>>> tryPumpFrom(
      ReadableStreamSource& input, bool end);

  virtual void abort(kj::Exception reason) = 0;
  // TODO(conform): abort() should return a promise after which closed fulfillers should be
  //   rejected. This may necessitate an "erroring" state.
};

class ReadableStreamSource {
public:
  virtual kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) = 0;

  // The ReadableStreamSource version of pumpTo() has no `amount` parameter, since the Streams spec
  // only defines pumping everything.
  //
  // If `end` is true, then `output.end()` will be called after pumping. Note that it's especially
  // important to take advantage of this when using deferred proxying since calling `end()`
  // directly might attempt to use the `IoContext` to call `registerPendingEvent()`.
  virtual kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end);

  virtual kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding);

  kj::Promise<kj::Array<byte>> readAllBytes(uint64_t limit);
  kj::Promise<kj::String> readAllText(uint64_t limit);

  // Hook to inform this ReadableStreamSource that the ReadableStream has been canceled. This only
  // really means anything to TransformStreams, which are supposed to propagate the error to the
  // writable side, and custom ReadableStreams, which we don't implement yet.
  //
  // NOTE: By "propagate the error back to the writable stream", I mean: if the WritableStream is in
  //   the Writable state, set it to the Errored state and reject its closed fulfiller with
  //   `reason`. I'm not sure how I'm going to do this yet.
  virtual void cancel(kj::Exception reason);
  // TODO(conform): Should return promise.
  //
  // TODO(conform): `reason` should be allowed to be any JS value, and not just an exception.
  //   That is, something silly like `stream.cancel(42)` should be allowed and trigger a
  //   rejection with the integer `42`.

  struct Tee {
    kj::Own<ReadableStreamSource> branches[2];
  };

  // Implement this if your ReadableStreamSource has a better way to tee a stream than the naive
  // method, which relies upon `tryRead()`. The default implementation returns nullptr.
  virtual kj::Maybe<Tee> tryTee(uint64_t limit);
};

struct PipeToOptions {
  jsg::Optional<bool> preventClose;
  jsg::Optional<bool> preventAbort;
  jsg::Optional<bool> preventCancel;
  jsg::Optional<jsg::Ref<AbortSignal>> signal;

  JSG_STRUCT(preventClose, preventAbort, preventCancel, signal);
  JSG_STRUCT_TS_OVERRIDE(StreamPipeOptions);

  // An additional, internal only property that is used to indicate
  // when the pipe operation is used for a pipeThrough rather than
  // a pipeTo. We use this information, for instance, to identify
  // when we should mark returned rejected promises as handled.
  bool pipeThrough = false;
};

namespace StreamStates {
  struct Closed {};
  using Errored = jsg::Value;
  struct Erroring {
    jsg::Value reason;

    Erroring(jsg::Value reason) : reason(kj::mv(reason)) {}
  };
}  // namespace StreamStates

// A ReadableStreamController provides the underlying implementation for a ReadableStream.
// We will generally have three implementations:
//  * ReadableStreamDefaultController
//  * ReadableByteStreamController
//  * ReadableStreamInternalController
//
// The ReadableStreamDefaultController and ReadableByteStreamController are defined by the
// streams standard and source all of the stream data from JavaScript functions provided by
// user code.
//
// The ReadableStreamInternalController is Workers runtime specific and provides a bridge
// to the existing ReadableStreamSource API. At the API contract layer, the
// ReadableByteStreamController and ReadableStreamInternalController will appear to be
// identical. Internally, however, they will be very different from one another.
//
// The ReadableStreamController instance is meant to be a private member of the ReadableStream,
// e.g.
//    class ReadableStream {
//    public:
//      // ...
//    private:
//      ReadableStreamController controller;
//      // ...
//    }
//
// As such, it exists within the V8 heap (it's allocated directly as a member of the
// ReadableStream) and will always execute within the V8 isolate lock.
//
// The methods here return jsg::Promise rather than kj::Promise because the controller
// operations here do not always require passing through the kj mechanisms or kj event loop.
// Likewise, we do not make use of kj::Exception in these interfaces because the stream
// standard dicates that streams can be canceled/aborted/errored using any arbitrary JavaScript
// value, not just Errors.
class ReadableStreamController {
public:
  // The ReadableStreamController::Reader interface is a base for all ReadableStream reader
  // implementations and is used solely as a means of attaching a Reader implementation to
  // the internal state of the controller. See the ReadableStream::*Reader classes for the
  // full Reader API.
  class Reader {
  public:
    // True if the reader is a BYOB reader.
    virtual bool isByteOriented() const = 0;

    // When a Reader is locked to a controller, the controller will attach itself to the reader,
    // passing along the closed promise that will be used to communicate state to the
    // user code.
    //
    // The Reader will hold a reference to the controller that will be cleared when the reader
    // is released or destroyed. The controller is guaranteed to either outlive or detach the
    // reader so the ReadableStreamController& reference should remain valid.
    virtual void attach(
        ReadableStreamController& controller,
        jsg::Promise<void> closedPromise) = 0;

    // When a Reader lock is released, the controller will signal to the reader that it has been
    // detached.
    virtual void detach() = 0;
  };

  struct ByobOptions {
    static constexpr size_t DEFAULT_AT_LEAST = 1;

    jsg::V8Ref<v8::ArrayBufferView> bufferView;
    size_t byteOffset = 0;
    size_t byteLength;

    // The minimum number of bytes that should be read. When not specified, the default
    // is DEFAULT_AT_LEAST. This is a non-standard, Workers-specific extension to
    // support the readAtLeast method on the ReadableStreamBYOBReader object.
    kj::Maybe<size_t> atLeast = DEFAULT_AT_LEAST;

    // True if the given buffer should be detached. Per the spec, we should always be
    // detaching a BYOB buffer but the original Workers implementation did not.
    // To avoid breaking backwards compatibility, a compatibility flag is provided to turn
    // detach on/off as appropriate.
    bool detachBuffer = true;
  };

  struct Tee {
    jsg::Ref<ReadableStream> branch1;
    jsg::Ref<ReadableStream> branch2;
  };

  // Abstract API for ReadableStreamController implementations that provide their own
  // tee implementations that are not backed by kj's tee. Each branch of the tee uses
  // the TeeController to interface with the shared underlying source, and the
  // TeeController ensures that each Branch receives the data that is read.
  class TeeController {
  public:
    // Represents an individual ReadableStreamController tee branch registered with
    // a TeeController. One or more branches is registered with the TeeController.
    class Branch {
    public:
      virtual ~Branch() noexcept(false) {}

      virtual void doClose(jsg::Lock& js) = 0;
      virtual void doError(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;
      virtual void handleData(jsg::Lock& js, ReadResult result) = 0;
    };

    class BranchPtr {
    public:
      inline BranchPtr(Branch* branch) : inner(branch) {
        KJ_ASSERT(inner != nullptr);
      }
      BranchPtr(BranchPtr&& other) = default;
      BranchPtr& operator=(BranchPtr&&) = default;
      BranchPtr(BranchPtr& other) = default;
      BranchPtr& operator=(BranchPtr&) = default;

      inline void doClose(jsg::Lock& js) { inner->doClose(js); }

      inline void doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
        inner->doError(js, reason);
      }

      inline void handleData(jsg::Lock& js, ReadResult result) {
        inner->handleData(js, kj::mv(result));
      }

      inline uint hashCode() { return kj::hashCode(inner); }
      inline bool operator==(BranchPtr& other) const {
        return inner == other.inner;
      }
    private:
      Branch* inner;
    };

    virtual ~TeeController() noexcept(false) {}

    virtual void addBranch(Branch* branch) = 0;

    virtual void close(jsg::Lock& js) = 0;

    virtual void error(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;

    virtual void ensurePulling(jsg::Lock& js) = 0;

    // maybeJs will be nullptr when the isolate lock is not available.
    // If maybeJs is set, any operations pending for the branch will be canceled.
    virtual void removeBranch(Branch* branch, kj::Maybe<jsg::Lock&> maybeJs) = 0;
  };

  // The PipeController simplifies the abstraction between ReadableStreamController
  // and WritableStreamController so that the pipeTo/pipeThrough/tryPipeTo can work
  // without caring about what kind of controller it is working with.
  class PipeController {
  public:
    virtual ~PipeController() noexcept(false) {}
    virtual bool isClosed() = 0;
    virtual kj::Maybe<v8::Local<v8::Value>> tryGetErrored(jsg::Lock& js) = 0;
    virtual void cancel(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;
    virtual void close(jsg::Lock& js) = 0;
    virtual void error(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;
    virtual void release(jsg::Lock& js,
                         kj::Maybe<v8::Local<v8::Value>> maybeError = nullptr) = 0;
    virtual kj::Maybe<kj::Promise<void>> tryPumpTo(WritableStreamSink& sink, bool end) = 0;
    virtual jsg::Promise<ReadResult> read(jsg::Lock& js) = 0;
  };

  virtual ~ReadableStreamController() noexcept(false) {}

  virtual void setOwnerRef(ReadableStream& stream) = 0;

  virtual jsg::Ref<ReadableStream> addRef() = 0;

  // Returns true if the underlying source for this controller is byte-oriented and
  // therefore supports the pull into API. When false, the stream can be used to pass
  // any arbitrary JavaScript value through.
  virtual bool isByteOriented() const = 0;

  // Reads data from the stream. If the stream is byte-oriented, then the ByobOptions can be
  // specified to provide a v8::ArrayBuffer to be filled by the read operation. If the ByobOptions
  // are provided and the stream is not byte-oriented, the operation will return a rejected promise.
  virtual kj::Maybe<jsg::Promise<ReadResult>> read(
      jsg::Lock& js,
      kj::Maybe<ByobOptions> byobOptions) = 0;

  // The pipeTo implementation fully consumes the stream by directing all of its data at the
  // destination. Controllers should try to be as efficient as possible here. For instance, if
  // a ReadableStreamInternalController is piping to a WritableStreamInternalController, then
  // a more efficient kj pipe should be possible.
  virtual jsg::Promise<void> pipeTo(
      jsg::Lock& js,
      WritableStreamController& destination,
      PipeToOptions options) = 0;

  // Indicates that the consumer no longer has any interest in the streams data.
  virtual jsg::Promise<void> cancel(
      jsg::Lock& js,
      jsg::Optional<v8::Local<v8::Value>> reason) = 0;

  // Branches the ReadableStreamController into two ReadableStream instances that will receive
  // this streams data. The specific details of how the branching occurs is entirely up to the
  // controller implementation.
  virtual Tee tee(jsg::Lock& js) = 0;

  virtual bool isClosedOrErrored() const = 0;

  virtual bool isDisturbed() = 0;

  // True if a Reader has been locked to this controller.
  virtual bool isLockedToReader() const = 0;

  // Locks this controller to the given reader, returning true if the lock was successful, or false
  // if the controller was already locked.
  virtual bool lockReader(jsg::Lock& js, Reader& reader) = 0;

  // Removes the lock and releases the reader from this controller.
  // maybeJs will be nullptr when the isolate lock is not available.
  // If maybeJs is set, the reader's closed promise will be resolved.
  virtual void releaseReader(Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) = 0;

  virtual kj::Maybe<PipeController&> tryPipeLock(jsg::Ref<WritableStream> destination) = 0;

  virtual void visitForGc(jsg::GcVisitor& visitor) {};

  // Fully consumes the ReadableStream. If the stream is already locked to a reader or
  // errored, the returned JS promise will reject. If the stream is already closed, the
  // returned JS promise will resolve with a zero-length result. Importantly, this will
  // lock the stream and will fully consume it.
  //
  // limit specifies an upper maximum bound on the number of bytes permitted to be read.
  // The promise will reject if the read will produce more bytes than the limit.
  virtual jsg::Promise<kj::Array<byte>> readAllBytes(jsg::Lock& js, uint64_t limit) = 0;

  // Fully consumes the ReadableStream. If the stream is already locked to a reader or
  // errored, the returned JS promise will reject. If the stream is already closed, the
  // returned JS promise will resolve with a zero-length result. Importantly, this will
  // lock the stream and will fully consume it.
  //
  // limit specifies an upper maximum bound on the number of bytes permitted to be read.
  // The promise will reject if the read will produce more bytes than the limit.
  virtual jsg::Promise<kj::String> readAllText(jsg::Lock& js, uint64_t limit) = 0;

  virtual kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) = 0;

  virtual void setup(
      jsg::Lock& js,
      jsg::Optional<UnderlyingSource> maybeUnderlyingSource,
      jsg::Optional<StreamQueuingStrategy> maybeQueuingStrategy) {}

  virtual kj::Promise<DeferredProxy<void>> pumpTo(
    jsg::Lock& js, kj::Own<WritableStreamSink> sink, bool end) = 0;

  virtual kj::Own<ReadableStreamController> detach(jsg::Lock& js, bool ignoreDisturbed) = 0;
};

kj::Own<ReadableStreamController> newReadableStreamJsController();
kj::Own<ReadableStreamController> newReadableStreamInternalController(
    IoContext& ioContext,
    kj::Own<ReadableStreamSource> source);

// A WritableStreamController provides the underlying implementation for a WritableStream.
// We will generally have two implementations:
//  * WritableStreamDefaultController
//  * WritableStreamInternalController
//
// The WritableStreamDefaultController is defined by the streams standard and directs all
// of the stream data to JavaScript functions provided by user code.
//
// The WritableStreamInternalController is Workers runtime specific and provides a bridge
// to the existing WritableStreamSink API.
//
// The WritableStreamController instance is meant to be a private member of the WritableStream,
// e.g.
//   class WritableStream {
//   public:
//     // ...
//   private:
//     WritableStreamController controller;
//   };
//
// As such, it exists within the V8 heap  (it's allocated directly as a member of the
// WritableStream) and will always execute within the V8 isolate lock.
// Both the WritableStreamDefaultController and WritableStreamInternalController will support
// the removeSink() method that can be used to acquire a kj heap object that can be used to
// write data from outside of the isolate lock, however, when using the
// WritableStreamDefaultController, each write operation will require acquiring the isolate lock.
//
// The methods here return jsg::Promise rather than kj::Promise because the controller
// operations here do not always require passing through the kj mechanisms or kj event loop.
// Likewise, we do not make use of kj::Exception in these interfaces because the stream
// standard dicates that streams can be canceled/aborted/errored using any arbitrary JavaScript
// value, not just Errors.
class WritableStreamController {
public:
  // The WritableStreamController::Writer interface is a base for all WritableStream writer
  // implementations and is used solely as a means of attaching a Writer implementation to
  // the internal state of the controller. See the WritableStream::*Writer classes for the
  // full Writer API.
  class Writer {
  public:
    // When a Writer is locked to a controller, the controller will attach itself to the writer,
    // passing along the closed and ready promises that will be used to communicate state to the
    // user code.
    //
    // The controller is guaranteed to either outlive the Writer or will detach the Writer so the
    // WritableStreamController& reference should always remain valid.
    virtual void attach(
        WritableStreamController& controller,
        jsg::Promise<void> closedPromise,
        jsg::Promise<void> readyPromise) = 0;

    // When a Writer lock is released, the controller will signal to the writer that is has been
    // detached.
    virtual void detach() = 0;

    // The ready promise can be replaced whenever backpressure is signaled by the underlying
    // controller.
    virtual void replaceReadyPromise(jsg::Promise<void> readyPromise) = 0;
  };

  struct PendingAbort {
    kj::Maybe<jsg::Promise<void>::Resolver> resolver;
    jsg::Promise<void> promise;
    jsg::Value reason;
    bool reject = false;

    PendingAbort(jsg::Lock& js,
                 jsg::PromiseResolverPair<void> prp,
                 v8::Local<v8::Value> reason,
                 bool reject);

    PendingAbort(jsg::Lock& js, v8::Local<v8::Value> reason, bool reject);

    void complete(jsg::Lock& js);

    void fail(jsg::Lock& js, v8::Local<v8::Value> reason);

    inline jsg::Promise<void> whenResolved(jsg::Lock& js) {
      return promise.whenResolved(js);
    }

    inline jsg::Promise<void> whenResolved(auto&& func) {
      return promise.whenResolved(kj::fwd(func));
    }

    inline jsg::Promise<void> whenResolved(auto&& func, auto&& errFunc) {
      return promise.whenResolved(kj::fwd(func), kj::fwd(errFunc));
    }

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(resolver, promise, reason);
    }

    static kj::Maybe<PendingAbort> dequeue(kj::Maybe<PendingAbort>& maybePendingAbort);
  };

  virtual ~WritableStreamController() noexcept(false) {}

  virtual void setOwnerRef(WritableStream& stream) = 0;

  virtual jsg::Ref<WritableStream> addRef() = 0;

  // The controller implementation will determine what kind of JavaScript data
  // it is capable of writing, returning a rejected promise if the written
  // data type is not supported.
  virtual jsg::Promise<void> write(jsg::Lock& js,
                                    jsg::Optional<v8::Local<v8::Value>> value) = 0;

  // Indicates that no additional data will be written to the controller. All
  // existing pending writes should be allowed to complete.
  virtual jsg::Promise<void> close(jsg::Lock& js, bool markAsHandled = false) = 0;

  // Waits for pending data to be written. The returned promise is resolved when all pending writes
  // have completed.
  virtual jsg::Promise<void> flush(jsg::Lock& js, bool markAsHandled = false) = 0;

  // Immediately interrupts existing pending writes and errors the stream.
  virtual jsg::Promise<void> abort(
      jsg::Lock& js,
      jsg::Optional<v8::Local<v8::Value>> reason) = 0;

  // The tryPipeFrom attempts to establish a data pipe where source's data
  // is delivered to this WritableStreamController as efficiently as possible.
  virtual kj::Maybe<jsg::Promise<void>> tryPipeFrom(
      jsg::Lock& js,
      jsg::Ref<ReadableStream> source,
      PipeToOptions options) = 0;

  // Only byte-oriented WritableStreamController implementations will have a WritableStreamSink
  // that can be detached using removeSink. A nullptr should be returned by any controller that
  // does not support removing the sink. After the WritableStreamSink has been released, all other
  // methods on the controller should fail with an exception as the WritableStreamSink should be
  // the only way to interact with the underlying sink.
  virtual kj::Maybe<kj::Own<WritableStreamSink>> removeSink(jsg::Lock& js) = 0;

  virtual kj::Maybe<int> getDesiredSize() = 0;

  // True if a Writer has been locked to this controller.
  virtual bool isLockedToWriter() const = 0;

  // Locks this controller to the given writer, returning true if the lock was successful, or false
  // if the controller was already locked.
  virtual bool lockWriter(jsg::Lock& js, Writer& writer) = 0;

  // Removes the lock and releases the writer from this controller.
  // maybeJs will be nullptr when the isolate lock is not available.
  // If maybeJs is set, the writer's closed and ready promises will be resolved.
  virtual void releaseWriter(Writer& writer, kj::Maybe<jsg::Lock&> maybeJs) = 0;

  virtual kj::Maybe<v8::Local<v8::Value>> isErroring(jsg::Lock& js) = 0;

  virtual void visitForGc(jsg::GcVisitor& visitor) {};

  virtual void setup(jsg::Lock& js,
                     jsg::Optional<UnderlyingSink> underlyingSink,
                     jsg::Optional<StreamQueuingStrategy> queuingStrategy) {}

  virtual bool isClosedOrClosing() = 0;
};

kj::Own<WritableStreamController> newWritableStreamJsController();
kj::Own<WritableStreamController> newWritableStreamInternalController(
    IoContext& ioContext,
    kj::Own<WritableStreamSink> source,
    kj::Maybe<uint64_t> maybeHighWaterMark = nullptr);

struct Unlocked {};
struct Locked {};

// When a reader is locked to a ReadableStream, a ReaderLock instance
// is used internally to represent the locked state in the ReadableStreamController.
class ReaderLocked {
public:
  ReaderLocked(
      ReadableStreamController::Reader& reader,
      jsg::Promise<void>::Resolver closedFulfiller,
      kj::Maybe<IoOwn<kj::Canceler>> canceler = nullptr)
      : reader(reader),
        closedFulfiller(kj::mv(closedFulfiller)),
        canceler(kj::mv(canceler)) {}

  ReaderLocked(ReaderLocked&&) = default;
  ~ReaderLocked() noexcept(false) {
    KJ_IF_MAYBE(r, reader) { r->detach(); }
  }
  KJ_DISALLOW_COPY(ReaderLocked);

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(closedFulfiller);
  }

  ReadableStreamController::Reader& getReader() {
    return KJ_ASSERT_NONNULL(reader);
  }

  kj::Maybe<jsg::Promise<void>::Resolver>& getClosedFulfiller() {
    return closedFulfiller;
  }

  kj::Maybe<IoOwn<kj::Canceler>>& getCanceler() {
    return canceler;
  }

private:
  kj::Maybe<ReadableStreamController::Reader&> reader;
  kj::Maybe<jsg::Promise<void>::Resolver> closedFulfiller;
  kj::Maybe<IoOwn<kj::Canceler>> canceler;
};

// When a writer is locked to a WritableStream, a WriterLock instance
// is used internally to represent the locked state in the WritableStreamController.
class WriterLocked {
public:
  WriterLocked(
      WritableStreamController::Writer& writer,
      jsg::Promise<void>::Resolver closedFulfiller,
      kj::Maybe<jsg::Promise<void>::Resolver> readyFulfiller = nullptr)
      : writer(writer),
        closedFulfiller(kj::mv(closedFulfiller)),
        readyFulfiller(kj::mv(readyFulfiller)) {}

  WriterLocked(WriterLocked&&) = default;
  ~WriterLocked() noexcept(false) {
    KJ_IF_MAYBE(w, writer) { w->detach(); }
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(closedFulfiller, readyFulfiller);
  }

  WritableStreamController::Writer& getWriter() {
    return KJ_ASSERT_NONNULL(writer);
  }

  kj::Maybe<jsg::Promise<void>::Resolver>& getClosedFulfiller() {
    return closedFulfiller;
  }

  kj::Maybe<jsg::Promise<void>::Resolver>& getReadyFulfiller() {
    return readyFulfiller;
  }

  void setReadyFulfiller(jsg::PromiseResolverPair<void>& pair) {
    KJ_IF_MAYBE(w, writer) {
      readyFulfiller = kj::mv(pair.resolver);
      w->replaceReadyPromise(kj::mv(pair.promise));
    }
  }

private:
  kj::Maybe<WritableStreamController::Writer&> writer;
  kj::Maybe<jsg::Promise<void>::Resolver> closedFulfiller;
  kj::Maybe<jsg::Promise<void>::Resolver> readyFulfiller;
};

template <typename T>
void maybeResolvePromise(
    jsg::Lock& js,
    kj::Maybe<typename jsg::Promise<T>::Resolver>& maybeResolver,
    T&& t) {
  KJ_IF_MAYBE(resolver, maybeResolver) {
    resolver->resolve(js, kj::fwd<T>(t));
    maybeResolver = nullptr;
  }
}

inline void maybeResolvePromise(
    jsg::Lock& js,
    kj::Maybe<typename jsg::Promise<void>::Resolver>& maybeResolver) {
  KJ_IF_MAYBE(resolver, maybeResolver) {
    resolver->resolve(js);
    maybeResolver = nullptr;
  }
}

template <typename T>
void maybeRejectPromise(
    jsg::Lock& js,
    kj::Maybe<typename jsg::Promise<T>::Resolver>& maybeResolver,
    v8::Local<v8::Value> reason) {
  KJ_IF_MAYBE(resolver, maybeResolver) {
    resolver->reject(js, reason);
    maybeResolver = nullptr;
  }
}

template <typename T>
jsg::Promise<T> rejectedMaybeHandledPromise(
    jsg::Lock& js,
    v8::Local<v8::Value> reason,
    bool handled) {
  auto prp = js.newPromiseAndResolver<T>();
  if (handled) {
    prp.promise.markAsHandled(js);
  }
  prp.resolver.reject(js, reason);
  return kj::mv(prp.promise);
}

inline kj::Maybe<IoContext&> tryGetIoContext() {
  if (IoContext::hasCurrent()) {
    return IoContext::current();
  }
  return nullptr;
}

}  // namespace workerd::api
