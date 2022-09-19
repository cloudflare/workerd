// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <kj/async-io.h>
#include <kj/vector.h>
#include <workerd/io/io-context.h>
#include "../basics.h"
#include "../util.h"

#include <deque>
#include <queue>

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

enum class StreamEncoding {
  IDENTITY,
  GZIP
};

struct ReadResult {
  jsg::Optional<jsg::Value> value;
  bool done;

  JSG_STRUCT(value, done);

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(value);
  }
};

struct PipeToOptions {
  jsg::Optional<bool> preventClose;
  jsg::Optional<bool> preventAbort;
  jsg::Optional<bool> preventCancel;
  jsg::Optional<jsg::Ref<AbortSignal>> signal;

  JSG_STRUCT(preventClose, preventAbort, preventCancel, signal);

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

class ReadableStreamController {
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
  // Both the ReadableStreamInternalController and ReadableByteStreamController support the
  // removeSource() method that can be used to acquire a kj heap object that can be used to
  // consume the stream data from outside of the isolate lock (however, when using the
  // ReadableByteStreamController, every tryRead() call will necessarily acquire the isolate
  // lock in order to complete).
  //
  // The methods here return jsg::Promise rather than kj::Promise because the controller
  // operations here do not always require passing through the kj mechanisms or kj event loop.
  // Likewise, we do not make use of kj::Exception in these interfaces because the stream
  // standard dicates that streams can be canceled/aborted/errored using any arbitrary JavaScript
  // value, not just Errors.
public:
  class Reader {
    // The ReadableStreamController::Reader interface is a base for all ReadableStream reader
    // implementations and is used solely as a means of attaching a Reader implementation to
    // the internal state of the controller. See the ReadableStream::*Reader classes for the
    // full Reader API.
  public:
    virtual bool isByteOriented() const = 0;
    // True if the reader is a BYOB reader.

    virtual void attach(
        ReadableStreamController& controller,
        jsg::Promise<void> closedPromise) = 0;
    // When a Reader is locked to a controller, the controller will attach itself to the reader,
    // passing along the closed promise that will be used to communicate state to the
    // user code.
    //
    // The Reader will hold a reference to the controller that will be cleared when the reader
    // is released or destroyed. The controller is guaranteed to either outlive or detach the
    // reader so the ReadableStreamController& reference should remain valid.

    virtual void detach() = 0;
    // When a Reader lock is released, the controller will signal to the reader that it has been
    // detached.
  };

  struct ByobOptions {
    static constexpr size_t DEFAULT_AT_LEAST = 1;

    jsg::V8Ref<v8::ArrayBufferView> bufferView;
    size_t byteOffset = 0;
    size_t byteLength;

    kj::Maybe<size_t> atLeast = DEFAULT_AT_LEAST;
    // The minimum number of bytes that should be read. When not specified, the default
    // is DEFAULT_AT_LEAST. This is a non-standard, Workers-specific extension to
    // support the readAtLeast method on the ReadableStreamBYOBReader object.

    bool detachBuffer = true;
    // True if the given buffer should be detached. Per the spec, we should always be
    // detaching a BYOB buffer but the original Workers implementation did not.
    // To avoid breaking backwards compatibility, a feature flag is provided to turn
    // detach on/off as appropriate.
  };

  struct Tee {
    jsg::Ref<ReadableStream> branch1;
    jsg::Ref<ReadableStream> branch2;
  };

  class TeeController {
    // Abstract API for ReadableStreamController implementations that provide their own
    // tee implementations that are not backed by kj's tee. Each branch of the tee uses
    // the TeeController to interface with the shared underlying source, and the
    // TeeController ensures that each Branch receives the data that is read.
  public:
    class Branch {
      // Represents an individual ReadableStreamController tee branch registered with
      // a TeeController. One or more branches is registered with the TeeController.
    public:
      virtual ~Branch() noexcept(false) {}

      virtual void doClose() = 0;
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

      inline void doClose() { inner->doClose(); }

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

    virtual void close() = 0;

    virtual void error(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;

    virtual void ensurePulling(jsg::Lock& js) = 0;

    virtual void removeBranch(Branch* branch, kj::Maybe<jsg::Lock&> maybeJs) = 0;
    // maybeJs will be nullptr when the isolate lock is not available.
    // If maybeJs is set, any operations pending for the branch will be canceled.
  };

  class PipeController {
    // The PipeController simplifies the abstraction between ReadableStreamController
    // and WritableStreamController so that the pipeTo/pipeThrough/tryPipeTo can work
    // without caring about what kind of controller it is working with.
  public:
    virtual ~PipeController() noexcept(false) {}
    virtual bool isClosed() = 0;
    virtual kj::Maybe<v8::Local<v8::Value>> tryGetErrored(jsg::Lock& js) = 0;
    virtual void cancel(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;
    virtual void close() = 0;
    virtual void error(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;
    virtual void release(jsg::Lock& js,
                         kj::Maybe<v8::Local<v8::Value>> maybeError = nullptr) = 0;
    virtual kj::Maybe<kj::Promise<void>> tryPumpTo(WritableStreamSink& sink, bool end) = 0;
    virtual jsg::Promise<ReadResult> read(jsg::Lock& js) = 0;
  };

  virtual ~ReadableStreamController() noexcept(false) {}

  virtual void setOwnerRef(ReadableStream& stream) = 0;

  virtual jsg::Ref<ReadableStream> addRef() = 0;

  virtual bool isByteOriented() const = 0;
  // Returns true if the underlying source for this controller is byte-oriented and
  // therefore supports the pull into API. When false, the stream can be used to pass
  // any arbitrary JavaScript value through.

  virtual kj::Maybe<jsg::Promise<ReadResult>> read(
      jsg::Lock& js,
      kj::Maybe<ByobOptions> byobOptions) = 0;
  // Reads data from the stream. If the stream is byte-oriented, then the ByobOptions can be
  // specified to provide a v8::ArrayBuffer to be filled by the read operation. If the ByobOptions
  // are provided and the stream is not byte-oriented, the operation will return a rejected promise.

  virtual jsg::Promise<void> pipeTo(
      jsg::Lock& js,
      WritableStreamController& destination,
      PipeToOptions options) = 0;
  // The pipeTo implementation fully consumes the stream by directing all of its data at the
  // destination. Controllers should try to be as efficient as possible here. For instance, if
  // a ReadableStreamInternalController is piping to a WritableStreamInternalController, then
  // a more efficient kj pipe should be possible.

  virtual jsg::Promise<void> cancel(
      jsg::Lock& js,
      jsg::Optional<v8::Local<v8::Value>> reason) = 0;
  // Indicates that the consumer no longer has any interest in the streams data.

  virtual Tee tee(jsg::Lock& js) = 0;
  // Branches the ReadableStreamController into two ReadableStream instances that will receive
  // this streams data. The specific details of how the branching occurs is entirely up to the
  // controller implementation.

  virtual kj::Maybe<kj::Own<ReadableStreamSource>> removeSource(jsg::Lock& js) = 0;
  // Only byte-oriented ReadableStreamController implementations will have a ReadableStreamSource
  // that can be detached using removeSource. A nullptr should be returned by controllers that do
  // not support removing the source. Once the source has been removed successfully, all other
  // operations on the controller should fail with an exception as the released ReadableStreamSource
  // should be the only way of interacting with the stream.

  virtual bool isClosedOrErrored() const = 0;

  virtual bool isDisturbed() = 0;

  virtual bool isLockedToReader() const = 0;
  // True if a Reader has been locked to this controller.

  virtual bool lockReader(jsg::Lock& js, Reader& reader) = 0;
  // Locks this controller to the given reader, returning true if the lock was successful, or false
  // if the controller was already locked.

  virtual void releaseReader(Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) = 0;
  // Removes the lock and releases the reader from this controller.
  // maybeJs will be nullptr when the isolate lock is not available.
  // If maybeJs is set, the reader's closed promise will be resolved.

  virtual kj::Maybe<PipeController&> tryPipeLock(jsg::Ref<WritableStream> destination) = 0;

  virtual void visitForGc(jsg::GcVisitor& visitor) {};
};

class WritableStreamController {
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
public:
  class Writer {
    // The WritableStreamController::Writer interface is a base for all WritableStream writer
    // implementations and is used solely as a means of attaching a Writer implementation to
    // the internal state of the controller. See the WritableStream::*Writer classes for the
    // full Writer API.
  public:
    virtual void attach(
        WritableStreamController& controller,
        jsg::Promise<void> closedPromise,
        jsg::Promise<void> readyPromise) = 0;
    // When a Writer is locked to a controller, the controller will attach itself to the writer,
    // passing along the closed and ready promises that will be used to communicate state to the
    // user code.
    //
    // The controller is guaranteed to either outlive the Writer or will detach the Writer so the
    // WritableStreamController& reference should always remain valid.

    virtual void detach() = 0;
    // When a Writer lock is released, the controller will signal to the writer that is has been
    // detached.

    virtual void replaceReadyPromise(jsg::Promise<void> readyPromise) = 0;
    // The ready promise can be replaced whenever backpressure is signaled by the underlying
    // controller.
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

    void fail(v8::Local<v8::Value> reason);

    inline jsg::Promise<void> whenResolved() {
      return promise.whenResolved();
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

  virtual jsg::Promise<void> write(jsg::Lock& js,
                                    jsg::Optional<v8::Local<v8::Value>> value) = 0;
  // The controller implementation will determine what kind of JavaScript data
  // it is capable of writing, returning a rejected promise if the written
  // data type is not supported.

  virtual jsg::Promise<void> close(jsg::Lock& js, bool markAsHandled = false) = 0;
  // Indicates that no additional data will be written to the controller. All
  // existing pending writes should be allowed to complete.

  virtual jsg::Promise<void> abort(
      jsg::Lock& js,
      jsg::Optional<v8::Local<v8::Value>> reason) = 0;
  // Immediately interrupts existing pending writes and errors the stream.

  virtual kj::Maybe<jsg::Promise<void>> tryPipeFrom(
      jsg::Lock& js,
      jsg::Ref<ReadableStream> source,
      PipeToOptions options) = 0;
  // The tryPipeFrom attempts to establish a data pipe where source's data
  // is delivered to this WritableStreamController as efficiently as possible.

  virtual kj::Maybe<kj::Own<WritableStreamSink>> removeSink(jsg::Lock& js) = 0;
  // Only byte-oriented WritableStreamController implementations will have a WritableStreamSink
  // that can be detached using removeSink. A nullptr should be returned by any controller that
  // does not support removing the sink. After the WritableStreamSink has been released, all other
  // methods on the controller should fail with an exception as the WritableStreamSink should be
  // the only way to interact with the underlying sink.

  virtual kj::Maybe<int> getDesiredSize() = 0;

  virtual bool isLockedToWriter() const = 0;
  // True if a Writer has been locked to this controller.

  virtual bool lockWriter(jsg::Lock& js, Writer& writer) = 0;
  // Locks this controller to the given writer, returning true if the lock was successful, or false
  // if the controller was already locked.

  virtual void releaseWriter(Writer& writer, kj::Maybe<jsg::Lock&> maybeJs) = 0;
  // Removes the lock and releases the writer from this controller.
  // maybeJs will be nullptr when the isolate lock is not available.
  // If maybeJs is set, the writer's closed and ready promises will be resolved.

  virtual kj::Maybe<v8::Local<v8::Value>> isErroring(jsg::Lock& js) = 0;

  virtual void visitForGc(jsg::GcVisitor& visitor) {};
};

struct Unlocked {};
struct Locked {};

class ReaderLocked {
  // When a reader is locked to a ReadableStream, a ReaderLock instance
  // is used internally to represent the locked state in the ReadableStreamController.
public:
  ReaderLocked(
      ReadableStreamController::Reader& reader,
      jsg::Promise<void>::Resolver closedFulfiller,
      kj::Maybe<IoOwn<kj::Canceler>> canceler = nullptr)
      : impl(Impl {
          .reader = reader,
          .closedFulfiller = kj::mv(closedFulfiller),
          .canceler = kj::mv(canceler)}) {}

  ReaderLocked(ReaderLocked&&) = default;
  ~ReaderLocked() noexcept(false) {
    KJ_IF_MAYBE(i, impl) {
      i->reader.detach();
    }
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_MAYBE(i, impl) {
      visitor.visit(i->closedFulfiller);
    }
  }

  ReadableStreamController::Reader& getReader() {
    return KJ_ASSERT_NONNULL(impl).reader;
  }

  kj::Maybe<jsg::Promise<void>::Resolver>& getClosedFulfiller() {
    return KJ_ASSERT_NONNULL(impl).closedFulfiller;
  }

  kj::Maybe<IoOwn<kj::Canceler>>& getCanceler() {
    return KJ_ASSERT_NONNULL(impl).canceler;
  }

private:
  struct Impl {
    ReadableStreamController::Reader& reader;
    kj::Maybe<jsg::Promise<void>::Resolver> closedFulfiller;
    kj::Maybe<IoOwn<kj::Canceler>> canceler;
  };
  kj::Maybe<Impl> impl;
};

class WriterLocked {
  // When a writer is locked to a WritableStream, a WriterLock instance
  // is used internally to represent the locked state in the WritableStreamController.
public:
  WriterLocked(
      WritableStreamController::Writer& writer,
      jsg::Promise<void>::Resolver closedFulfiller,
      kj::Maybe<jsg::Promise<void>::Resolver> readyFulfiller = nullptr)
      : impl(Impl {
          .writer = writer,
          .closedFulfiller = kj::mv(closedFulfiller),
          .readyFulfiller = kj::mv(readyFulfiller) }) {}

  WriterLocked(WriterLocked&&) = default;
  ~WriterLocked() noexcept(false) {
    KJ_IF_MAYBE(i, impl) {
      i->writer.detach();
    }
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_MAYBE(i, impl) {
      visitor.visit(i->closedFulfiller, i->readyFulfiller);
    }
  }

  WritableStreamController::Writer& getWriter() {
    return KJ_ASSERT_NONNULL(impl).writer;
  }

  kj::Maybe<jsg::Promise<void>::Resolver>& getClosedFulfiller() {
    return KJ_ASSERT_NONNULL(impl).closedFulfiller;
  }

  kj::Maybe<jsg::Promise<void>::Resolver>& getReadyFulfiller() {
    return KJ_ASSERT_NONNULL(impl).readyFulfiller;
  }

  void setReadyFulfiller(jsg::PromiseResolverPair<void>& pair) {
    KJ_IF_MAYBE(i, impl) {
      i->readyFulfiller = kj::mv(pair.resolver);
      i->writer.replaceReadyPromise(kj::mv(pair.promise));
    }
  }

private:
  struct Impl {
    WritableStreamController::Writer& writer;
    kj::Maybe<jsg::Promise<void>::Resolver> closedFulfiller;
    kj::Maybe<jsg::Promise<void>::Resolver> readyFulfiller;
  };
  kj::Maybe<Impl> impl;
};

template <typename T>
void maybeResolvePromise(
    kj::Maybe<typename jsg::Promise<T>::Resolver>& maybeResolver,
    T&& t) {
  KJ_IF_MAYBE(resolver, maybeResolver) {
    resolver->resolve(kj::fwd<T>(t));
    maybeResolver = nullptr;
  }
}

inline void maybeResolvePromise(
    kj::Maybe<typename jsg::Promise<void>::Resolver>& maybeResolver) {
  KJ_IF_MAYBE(resolver, maybeResolver) {
    resolver->resolve();
    maybeResolver = nullptr;
  }
}

template <typename T>
void maybeRejectPromise(
    kj::Maybe<typename jsg::Promise<T>::Resolver>& maybeResolver,
    v8::Local<v8::Value> reason) {
  KJ_IF_MAYBE(resolver, maybeResolver) {
    resolver->reject(reason);
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
    prp.promise.markAsHandled();
  }
  prp.resolver.reject(reason);
  return kj::mv(prp.promise);
}

}  // namespace workerd::api
