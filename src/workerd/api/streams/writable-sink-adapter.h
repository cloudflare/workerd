#include "common.h"

#include <workerd/util/weak-refs.h>

namespace workerd::api::streams {
// Wraps a WritableStreamSink with a more JS-friendly interface that implements
// queued writes and backpressure signaling. This is arguably what WritableStreamSink
// should have been in the first place. Eventually we might be able to replace
// WritableStreamSink with this class directly, but for now we need to keep both.
//
// Instances of WritableStreamSinkJsAdapter are meant to be used from within the
// isolate lock, when you have need to write data to a kj stream from JavaScript.
// As such, it is not a jsg::Object itself, nor is it a kj I/O object, but it
// sits between the two worlds. Internally it holds the WritableStreamSink within
// an IoOwn so that correct IoContext usage is enforced. But the kj::Own for the
// adapter itself is meant to be held in JS land.
//
// Once created, the adapter owns the underlying WritableStreamSink. It is not
// possible to extract the sink from the adapter. This is because the adapter
// needs to be able to enforce its own state machine and queued write mechanism.
//
// The adapter implements backpressure signaling based on a high water mark
// configured at construction time. When the number of bytes in flight exceeds
// the high water mark, we signal backpressure by causing the ready promise
// to be reset to a new pending promise. When backpressure is released again,
// the ready promise is resolved. The identity of the ready promise changes
// whenever the backpressure state changes.
//
// The adapter also implements flush signaling. Flushing signals are checkpoints
// that are inserted into the write queue, essentially like a no-op write. They
// can be used as synchronization points to ensure that all prior writes have
// completed. Flush signals do not affect backpressure or stream state.
//
// Dropping the adapter will cancel any in-flight and pending operations
// immediately. Dropping the IoContext while the adapter is still active
// will also cancel any in-flight and pending operations and cause the
// adapter to be invalidated (the Active state is held with an IoOwn).
//
//     ┌───────────────────────────────────────────┐
//     │         JavaScript Code                   │
//     │                                           │
//     │  • write(data) → Promise<void>            │
//     │  • flush() → Promise<void>                │
//     │  • end() → Promise<void>                  │
//     │  • abort(reason)                          │
//     │  • getReady() → Promise<void>             │
//     └───────────────────────────────────────────┘
//                            │
//                            ▼
//     ┌───────────────────────────────────────────┐
//     │    WritableStreamSinkJsAdapter            │
//     │                                           │
//     │  ┌─────────────────────────────────────┐  │
//     │  │       JavaScript API                │  │
//     │  │                                     │  │
//     │  │  • write(data) → Promise<void>      │  │
//     │  │  • flush() → Promise<void>          │  │
//     │  │  • end() → Promise<void>            │  │
//     │  │  • abort(reason)                    │  │
//     │  │  • getReady() → Promise<void>       │  │
//     │  │  • getDesiredSize() → number        │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │      Backpressure Management        │  │
//     │  │                                     │  │
//     │  │  • High water mark (16KB default)   │  │
//     │  │  • Bytes in flight tracking         │  │
//     │  │  • Ready promise signaling          │  │
//     │  │  • Queue depth management           │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │       Write Queue Management        │  │
//     │  │                                     │  │
//     │  │  • Queued writes with ordering      │  │
//     │  │  • Flush checkpoints                │  │
//     │  │  • Single in-flight write           │  │
//     │  │  • Error propagation                │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │         KJ Integration              │  │
//     │  │                                     │  │
//     │  │  IoOwn<WritableStreamSink>          │  │
//     │  │  WeakRef for safe references        │  │
//     │  │  IoContext-aware operations         │  │
//     │  └─────────────────────────────────────┘  │
//     └───────────────────────────────────────────┘
//                            │
//                            ▼
//     ┌───────────────────────────────────────────┐
//     │       WritableStreamSink                  │
//     │       (KJ Native Sink)                    │
//     │                                           │
//     │  • write(buffer) → Promise<void>          │
//     │  • end() → Promise<void>                  │
//     │  • abort(reason)                          │
//     └───────────────────────────────────────────┘
//
class WritableStreamSinkJsAdapter final {
 public:
  struct Options {
    // While the WritableStreamSink interface, and kj streams in general, do
    // not have a notion of backpressure, and instead generally require only
    // one write to be in flight at a time, it's better for performance for
    // us to be able to buffer a bit more data in flight. So we will implement
    // a simple high water mark mechanism. The default is 16KB.
    size_t highWaterMark = 16384;

    // When detachOnWrite is true, and a write() is made with an ArrayBuffer,
    // or ArrayBufferView, we will attempt to detach the underlying buffer
    // before writing it to the sink. Detaching is required by the
    // streams spec but our original implementation does not detach
    // and it turns out there are old workers depending on that behavior.
    bool detachOnWrite = false;
  };

  WritableStreamSinkJsAdapter(jsg::Lock& js,
      IoContext& ioContext,
      kj::Own<WritableStreamSink> sink,
      kj::Maybe<Options> options = kj::none);
  WritableStreamSinkJsAdapter(jsg::Lock& js,
      IoContext& ioContext,
      kj::Own<kj::AsyncOutputStream> stream,
      StreamEncoding encoding,
      kj::Maybe<Options> options = kj::none);
  KJ_DISALLOW_COPY_AND_MOVE(WritableStreamSinkJsAdapter);
  ~WritableStreamSinkJsAdapter() noexcept(false);

  // If we are in the errored state, returns the exception, otherwise kj::none.
  kj::Maybe<const kj::Exception&> isErrored() KJ_LIFETIMEBOUND;

  // Returns true if we are in the closed state.
  bool isClosed();

  // Returns true if close() has been called but we are not yet closed.
  bool isClosing();

  // If we are not in the closed or errored state, returns the desired
  // size based on the configured high water mark and the number of
  // bytes currently in flight. The desired size is the number of bytes
  // that can be written before we exceed the high water mark. If the
  // return value is <= 0 then backpressure is being signaled. If we are
  // in the closed or errored states, returns kj::none.
  kj::Maybe<ssize_t> getDesiredSize();

  // Writes a chunk to the underlying sink via the queued write mechanism.
  // The implementation ensures that only one write is in flight with the
  // underlying sink at a time, while additional writes are queued up
  // behind it. It is not necessary to await the returned promise before
  // calling write() again, though doing so is not an error. If the write
  // fails, the returned promise will reject with the failure reason.
  // Also if the write fails, the adapter will be transitioned to the
  // errored state and all subsequent queued writes will fail. Once
  // close() has been called, no additional writes will be accepted
  // and the returned promise will reject with an error. If the adapter
  // is already in the closed or errored state, the returned promise will
  // be rejected.
  //
  // Values written may be ArrayBuffer, ArrayBufferView, SharedArrayBuffer,
  // or string. Other types will cause the returned promise to reject.
  //
  // Backpressure is signaled when the number of bytes in flight (i.e.
  // the total number of bytes passed to write() calls that have not yet
  // completed) exceeds the configured high water mark. When backpressure
  // is signaled, additional writes are still accepted and queued up, but
  // the caller really should wait for the ready promise to resolve before
  // continuing to write more. This works exactly like a WritableStream's
  // backpressure mechanism. Callers keep writing until backpressure is
  // signaled, then wait for the ready promise to resolve before continuing,
  // etc.
  jsg::Promise<void> write(jsg::Lock& js, const jsg::JsValue& value);

  // Inserts a flush signal into the write queue. The returned promise
  // resolves once all prior writes have completed. This can be used
  // as a synchronization point to ensure that all writes up to this
  // point have been fully processed. If the adapter is in the closed
  // or errored state, the returned promise will reject. If the stream
  // errors while waiting for prior writes to complete, the returned
  // promise will be rejected.
  jsg::Promise<void> flush(jsg::Lock& js);

  // Transitions the adapter into the closing state. Once the write queue
  // is empty, we will close the sink and transition to the closed state.
  // If the adapter is already in the closing state, a new promise is
  // returned that will resolve when the adapter is fully closed. If the
  // adapter is already closed, a resolved promise is returned. If the
  // adapter is in the errored state, a rejected promise is returned.
  // All pending writes in the queue will be processed before closing
  // the sink and transitioning to the closed state. If any pending
  // writes fail, the adapter will transition to the errored state, and
  // all subsequent pending writes will be rejected along with the close
  // promise.
  jsg::Promise<void> end(jsg::Lock& js);

  // Transitions the adapter to the errored state, even if we are already closed.
  // All pending or in-flight writes, and a pending close, will all be rejected
  // with the given exception. If we are already in the errored state, this
  // is a no-op. This change is immediate. Once in the errored state, no
  // further writes or closes are allowed.
  void abort(kj::Exception&& exception);

  // Transitions the adapter to the errored state, even if we are already closed.
  // All pending or in-flight writes, and a pending close, will all be rejected
  // with the given exception. If we are already in the errored state, this
  // is a no-op. This change is immediate. Once in the errored state, no
  // further writes or closes are allowed. This variant is for use when
  // the exception is coming from JavaScript. It will be converted into a
  // tunneled kj::Exception.
  void abort(jsg::Lock& js, const jsg::JsValue& reason);

  // Returns a promise that resolves when backpressure is released.
  // Note that the identity of the returned promise will change as the
  // backpressure state changes. Whenever backpressure is signaled, a new
  // pending promise will be created, whenever backpressure is released
  // again that promise will be resolved. As such, this promise should
  // not be cached or stored. Instead, before every write() call, the
  // caller should wait on the current getReady() promise.
  jsg::Promise<void> getReady(jsg::Lock& js);

  // Returns a memoized identity for the ready promise. This can be used
  // to return a stable reference to the ready promise out to JavaScript
  // that will not change identity between calls unless the backpressure
  // state changes. Like the getReady() promise, this should not be cached
  // or stored, but it is safe to return this from a getter multiple times
  // to JavaScript as it will ensure that the same JS promise object is
  // always returned until the backpressure state changes. This variation
  // is not suitable for use within C++ code that needs to await on the
  // ready promise because the internal jsg::Promise<void> object will
  // no longer exist once the reference is passed out to JavaScript.
  jsg::MemoizedIdentity<jsg::Promise<void>>& getReadyStable();

  // Returns the options used to configure this adapter if the adapter
  // is not closed or errored.
  kj::Maybe<const Options&> getOptions();

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  // Represents the active state of the adapter. Importantly, this state
  // holds both the underlying WritableStreamSink and the write queue.
  // It must be held within an IoOwn.
  struct Active;
  struct Closed final {};
  kj::OneOf<IoOwn<Active>, Closed, kj::Exception> state;

  // Used for backpressure signaling. When backpressure is indicated, the
  // readyResolver, ready, and readyWatcher will be replaced with a new set.
  // When backpressure is relieved, the readyResolver will be resolved.
  // The adapter will start out in a ready state.
  struct BackpressureState final {
    // Note that if the BackpressureState is dropped while in a waiting state,
    // the ready promise will be left unresolved. This is OK.
    kj::Maybe<jsg::Promise<void>::Resolver> readyResolver;
    jsg::Promise<void> ready;
    jsg::MemoizedIdentity<jsg::Promise<void>> readyWatcher;

    // Aborts backpressure signaling, likely because the adapter is being errored.
    // Causes the ready promise to be rejected with the given reason.
    void abort(jsg::Lock& js, const jsg::JsValue& reason);

    // Releases backpressure, resolving the ready promise.
    void release(jsg::Lock& js);

    // Indicates that backpressure has been signaled and we are waiting
    // for it to be released or aborted.
    bool isWaiting() const;

    // Returns a promise that resolves when backpressure is released.
    // Note that every call to this returns a new jsg::Promise<void>
    // instance. Callers that need a stable identity should use
    // getReadyStable() instead (generally this is only the case when
    // returning the promise to JavaScript via a getter).
    jsg::Promise<void> getReady(jsg::Lock& js);

    // Returns a memoized identity for the ready promise. This can be used
    // to return a stable reference to the ready promise out to JavaScript
    // that will not change identity between calls unless the backpressure
    // state changes.
    jsg::MemoizedIdentity<jsg::Promise<void>>& getReadyStable();
    BackpressureState(jsg::Promise<void>::Resolver&& resolver,
        jsg::Promise<void>&& promise,
        jsg::MemoizedIdentity<jsg::Promise<void>>&& watcher);
  };
  BackpressureState backpressureState;
  kj::Rc<WeakRef<WritableStreamSinkJsAdapter>> selfRef;

  // Replaces the backpressure state with a new one, indicating that backpressure
  // is being applied. If we are already in a backpressure state, this is a no-op.
  // This will cause the ready promise (and its stable identity) to change.
  void maybeSignalBackpressure(jsg::Lock& js);

  // Conditionally releases backpressure if the desired size is now > 0.
  void maybeReleaseBackpressure(jsg::Lock& js);

  // Creates a new BackpressureState in the waiting state.
  static BackpressureState newBackpressureState(jsg::Lock& js);
};

// ================================================================================

// Adapts a WritableStream to a KJ-frendly interface.
// The adapter fully wraps the WritableStream instance,
// using a WritableStreamDefaultWriter to push data to it.
// Then the adapter is destroyed or aborted, the writer is
// aborted and both the writer and the stream references
// are dropped. Critically, the stream is not usable after
// ownership is transferred to this adapter. Initializing the adapter
// will fail if the stream is already locked.
//
// If the adapter is dropped, or aborted while there are pending writes,
// the pending writes will be rejected with the same exception as the abort.
//
// While WritableStream itself allows multiple writes to be in flight
// at the same time, the WritableStreamSink interface does not, so
// the adapter will ensure that only one write is in flight at a time.
//
// While the caller is expected to follow the WritableStreamSink contract
// and keep the adapter alive until the write promises resolve, there
// are some protections in place to avoid use-after-free if the caller
// drops the adapter. There's nothing we can do if the caller drops the
// buffer, however, so that is still a hard requirement.
// TODO(safety): This can be made safer by having write take a kj::Array
// as input instead of a kj::ArrayPtr but that's a larger refactor.
//
//     ┌───────────────────────────────────────────┐
//     │         KJ Native Code                    │
//     │                                           │
//     │  • write(buffer)                          │
//     │  • write(pieces[])                        │
//     │  • end()                                  │
//     │  • abort(reason)                          │
//     │  • tryPumpFrom(source, end)               │
//     └───────────────────────────────────────────┘
//                            │
//                            ▼
//     ┌───────────────────────────────────────────┐
//     │    WritableStreamSinkKjAdapter            │
//     │                                           │
//     │  ┌─────────────────────────────────────┐  │
//     │  │         KJ Native API               │  │
//     │  │                                     │  │
//     │  │  • write(ArrayPtr<byte>)            │  │
//     │  │  • write(ArrayPtr<ArrayPtr<byte>>)  │  │
//     │  │  • end() → Promise<void>            │  │
//     │  │  • abort(exception)                 │  │
//     │  │  • tryPumpFrom(source, end)         │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │       State Management              │  │
//     │  │                                     │  │
//     │  │   Active ──► Closed                 │  │
//     │  │     │          │                    │  │
//     │  │     │          ▼                    │  │
//     │  │     └─────► Errored                 │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │     JavaScript Integration          │  │
//     │  │                                     │  │
//     │  │  WritableStreamDefaultWriter        │  │
//     │  │  WeakRef for safe references        │  │
//     │  │  IoContext-aware JS operations      │  │
//     │  │  Promise handling & async writes    │  │
//     │  └─────────────────────────────────────┘  │
//     └───────────────────────────────────────────┘
//                            │
//                            ▼
//     ┌───────────────────────────────────────────┐
//     │      JavaScript WritableStream            │
//     │                                           │
//     │  • getWriter()                            │
//     │  • write(chunk) → Promise<void>           │
//     │  • close() → Promise<void>                │
//     │  • abort(reason) → Promise<void>          │
//     │  • locked, state properties               │
//     └───────────────────────────────────────────┘
//
class WritableStreamSinkKjAdapter final: public WritableStreamSink {
 public:
  WritableStreamSinkKjAdapter(jsg::Lock& js, IoContext& ioContext, jsg::Ref<WritableStream> stream);
  ~WritableStreamSinkKjAdapter() noexcept(false);

  // Attempts to write the given buffer to the underlying stream.
  // The returned promise resolves once the write has completed.
  // If the stream is closed, the returned promise rejects with
  // an exception. If the stream errors, the returned promise
  // rejects with the same exception. If the write fails, the
  // returned promise rejects with the failure reason.
  //
  // Per the contract of write, it is the caller's responsibility
  // to ensure that the adapter and buffer remain alive until
  // the returned promise resolves.
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override;

  // Attempts to write the given pieces to the underlying stream.
  // The returned promise resolves once the full write has completed.
  // If the stream is closed, the returned promise rejects with
  // an exception. If the stream errors, the returned promise
  // rejects with the same exception. If the write fails, the
  // returned promise rejects with the failure reason.
  // Per the contract of write, it is the caller's responsibility
  // to ensure that the adapter and buffers remain alive until
  // the returned promise resolves.
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override;

  // Closes the underlying stream. The returned promise resolves
  // once the stream is fully closed. If the stream is already
  // closed, the returned promise resolves immediately. If the
  // stream errors, the returned promise rejects with the same
  // exception. If the close fails, the returned promise rejects
  // with the failure reason.
  kj::Promise<void> end() override;

  // Attempts to establish a data pipe where input's data is delivered
  // to this WritableStreamSinkKjAdapter as efficiently as possible.
  kj::Maybe<kj::Promise<DeferredProxy<void>>> tryPumpFrom(
      ReadableStreamSource& input, bool end) override;

  // Immediately interrupts existing pending writes and errors the stream.
  // All pending or in-flight writes will be rejected with the given
  // exception. If we are already in the errored state, this is a no-op
  // and the exception is ignored. This change is immediate. Once in
  // the errored state, no further writes or closes are allowed.
  void abort(kj::Exception reason) override;

 private:
  struct Active;
  KJ_DECLARE_NON_POLYMORPHIC(Active);
  struct Closed {};
  kj::OneOf<kj::Own<Active>, Closed, kj::Exception> state;
  kj::Rc<WeakRef<WritableStreamSinkKjAdapter>> selfRef;

  kj::Promise<void> pumpFromImpl(ReadableStreamSource& input, bool end);
};

}  // namespace workerd::api::streams
