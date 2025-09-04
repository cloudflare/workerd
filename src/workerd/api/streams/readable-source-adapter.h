#include "common.h"
#include "readable.h"

namespace workerd::api::streams {

// We provide two utility adapters here: ReadableStreamSourceJsAdapter and
// ReadableStreamSourceKjAdapter.
//
// ReadableStreamSourceJsAdapter adapts a ReadableStreamSource to a JavaScript-friendly
// interface. It provides methods that return JavaScript promises and use
// JavaScript types. It is intended to be used by JavaScript code that wants
// to read from a kj-backed stream. It takes ownership of the ReadableStreamSource
// and holds it with an IoOwn, ensures that all operations are performed on the
// correct IoContext, and safely cleans up after itself if the adapter is dropped.
//
//     ┌───────────────────────────────────────────┐
//     │    ReadableStreamSourceJsAdapter          │
//     │                                           │
//     │  ┌─────────────────────────────────────┐  │
//     │  │          JavaScript API             │  │
//     │  │                                     │  │
//     │  │  • read() → Promise<ReadResult>     │  │
//     │  │  • readAllText() → Promise<string>  │  │
//     │  │  • readAllBytes() → Promise<bytes>  │  │
//     │  │  • close() → Promise<void>          │  │
//     │  │  • cancel(reason)                   │  │
//     │  │  • tryTee() → {branch1, branch2}    │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │         State Management            │  │
//     │  │                                     │  │
//     │  │   Active ──► Closed                 │  │
//     │  │     │          │                    │  │
//     │  │     │          ▼                    │  │
//     │  │     └─────► Canceled/Errored        │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │        KJ Integration               │  │
//     │  │                                     │  │
//     │  │  IoOwn<ReadableStreamSource>        │  │
//     │  │  WeakRef for safe references        │  │
//     │  │  IoContext-aware operations         │  │
//     │  └─────────────────────────────────────┘  │
//     └───────────────────────────────────────────┘
//                            │
//                            ▼
//     ┌───────────────────────────────────────────┐
//     │       ReadableStreamSource                │
//     │       (KJ Native Stream)                  │
//     │                                           │
//     │  • tryRead()                              │
//     │  • pumpTo()                               │
//     │  • tryGetLength()                         │
//     │  • cancel()                               │
//     └───────────────────────────────────────────┘
//
// The ReadableStreamSourceKjAdapter adapts a ReadableStream to a KJ-friendly
// ReadableStreamSource. It holds a strong reference to the ReadableStream and
// locks it with a ReadableStreamDefaultReader. It is intended to be used by
// KJ code that wants to read from a JavaScript-backed stream. It ensures that
// all operations are performed on the correct IoContext, and safely cleans up
// after itself if the adapter is dropped.
//
//     ┌───────────────────────────────────────────┐
//     │   ReadableStreamSourceKjAdapter           │
//     │                                           │
//     │  ┌─────────────────────────────────────┐  │
//     │  │         KJ Native API               │  │
//     │  │                                     │  │
//     │  │  • tryRead(minBytes, maxBytes)      │  │
//     │  │  • pumpTo(sink, end)                │  │
//     │  │  • tryGetLength(encoding)           │  │
//     │  │  • cancel(exception)                │  │
//     │  │  • getPreferredEncoding()           │  │
//     │  │  • tryTee() → none (unsupported)    │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │       State Management              │  │
//     │  │                                     │  │
//     │  │   Active ──► Closed                 │  │
//     │  │     │                               │  │
//     │  │     └─────► Canceled/Errored        │  │
//     │  └─────────────────────────────────────┘  │
//     │                   │                       │
//     │                   ▼                       │
//     │  ┌─────────────────────────────────────┐  │
//     │  │      JavaScript Integration         │  │
//     │  │                                     │  │
//     │  │  ReadableStreamDefaultReader        │  │
//     │  │  WeakRef for safe references        │  │
//     │  │  IoContext-aware JS operations      │  │
//     │  │  Promise handling & async reads     │  │
//     │  └─────────────────────────────────────┘  │
//     └───────────────────────────────────────────┘
//                            │
//                            ▼
//     ┌───────────────────────────────────────────┐
//     │       JavaScript ReadableStream           │
//     │                                           │
//     │  • getReader()                            │
//     │  • read() → Promise<{value, done}>        │
//     │  • cancel(reason)                         │
//     │  • locked, state properties               │
//     └───────────────────────────────────────────┘

// Adapts a ReadableStreamSource to a JavaScript-friendly interface.
class ReadableStreamSourceJsAdapter final {
 public:
  ReadableStreamSourceJsAdapter(
      jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> source);
  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamSourceJsAdapter);
  ~ReadableStreamSourceJsAdapter() noexcept(false);

  // Returns true if the adapter is closed or canceled.
  bool isClosed();

  // If the adapter is canceled, returns the exception it was
  // canceled with. Otherwise returns null.
  kj::Maybe<const kj::Exception&> isCanceled() KJ_LIFETIMEBOUND;

  // Cancels the underlying source if it is still active. If an
  // exception is provided, the source will be errored with that.
  // If no exception is provided, the source will be closed without
  // error. All in-flight and pending read requests will be rejected.
  // Unlike close(), the effect is immediate.
  void cancel(kj::Exception exception);

  // Like cancel() but with the error reason provided as a JS value.
  void cancel(jsg::Lock& js, const jsg::JsValue& reason);

  // Closes the stream immediatey without error if it is still
  // active. All in-flight and pending read requests will be
  // rejected with a cancelation error but the adapter will
  // transition to the closed state rather than the errored state.
  // If the adapter is already closed or canceled, this is a no-op.
  void shutdown(jsg::Lock& js);

  // Causes the adapter to enter the closing state. Any pending
  // read requests will be allowed to complete but no new read requests
  // will be accepted. The underlying source will be closed fully
  // once all pending reads complete. If cancel() has already been
  // called, this is a no-op and the returned promise resolves
  // immediately. If cancel() is called while this is pending,
  // the returned promise will reject with the same exception
  // and the cancel will supersede the close.
  jsg::Promise<void> close(jsg::Lock& js);

  struct ReadOptions {
    // The buffer to read into. The maximum number of bytes read
    // is equal to the length of this buffer. The actual number of
    // bytes read is indicated by the resolved value of the promise
    // but will never exceed the length of this buffer.
    jsg::BufferSource buffer;

    // The optional minimum number of bytes to read. If not provided,
    // the read will complete as soon as at least the mininum number
    // of bytes to satisfy the minimum bytes-per-element of the input
    // buffer is available.
    // It is often more efficient to provide a minimum number of bytes
    // because it allows to implementation to wait until larger chunks
    // of data are available before completing the read.
    kj::Maybe<size_t> minBytes;
  };
  struct ReadResult {
    // The buffer containing the data that was read. The length
    // of the buffer may be less than the length of the buffer
    // provided in ReadOptions if fewer bytes were available.
    // The identity of the underlying ArrayBuffer will be the same
    // but the buffer itself will be a new type array view.
    // of the same type as that provided in ReadOptions.
    // If the read produced no data because the stream is
    // closed, the type array will be zero length.
    jsg::BufferSource buffer;

    // True if the stream is now closed and no further reads
    // are possible. If this is true, the buffer will be zero
    // length.
    bool done = false;
  };

  // Submit a read request. The returned promise resolves with a
  // BufferSource containing the data that was read.
  jsg::Promise<ReadResult> read(jsg::Lock& js, ReadOptions options);

  // Utility function to read the entire stream as text. This is
  // terminal in that once this is called, no further reads
  // are possible. The entire stream will be read and concatenated
  // and the resulting string returned. If the stream errors while
  // reading, the promise will reject with the error.
  // If there are pending reads when this is called, those reads
  // will be allowed to complete first, and then the stream will
  // be read to the end.
  jsg::Promise<jsg::JsRef<jsg::JsString>> readAllText(jsg::Lock& js, uint64_t limit = kj::maxValue);

  // Utility function to read the entire stream as bytes. This is
  // terminal in that once this is called, no further reads
  // are possible. The entire stream will be read and concatenated
  // and the resulting bytes returned as a single BufferSource.
  // If the stream errors while reading, the promise will reject
  // with the error.
  // If there are pending reads when this is called, those reads
  // will be allowed to complete first, and then the stream will
  // be read to the end.
  jsg::Promise<jsg::BufferSource> readAllBytes(jsg::Lock& js, uint64_t limit = kj::maxValue);

  // If the stream is still active, tries to get the total length,
  // if known. If the length is not known, the encoding does not
  // match the encoding of the underlying stream, or the stream is
  // closed or errored, returns kj::none.
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding);

  struct Tee {
    kj::Own<ReadableStreamSourceJsAdapter> branch1;
    kj::Own<ReadableStreamSourceJsAdapter> branch2;
  };
  // Tees the stream into two branches. The returned Tee contains
  // two new ReadableStreamSourceJsAdapter instances that will
  // each receive the same data as this instance. Once this is called,
  // this instance is no longer usable and all further operations
  // on it will fail. Each branch operates independently; closing,
  // canceling, or erroring one branch has no effect on the other branch.
  // If this instance is already closed or canceled, or if there are
  // in-flight or pending reads, this will throw.
  kj::Maybe<Tee> tryTee(jsg::Lock& js, uint64_t limit = kj::maxValue);

 private:
  struct Active;
  struct Closed final {};
  kj::OneOf<IoOwn<Active>, Closed, kj::Exception> state;

  kj::Rc<WeakRef<ReadableStreamSourceJsAdapter>> selfRef;
};

// ===============================================================================================

// Adapts a ReadableStream to a KJ-friendly interface.
// The adapter fully wraps and consumes the ReadableStream instance,
// using a ReadableStreamDefaultReader to pull data from it.
// When the adapter is destroyed or canceled, the reader is canceled
// and both the reader and the stream references are dropped. Critically,
// the stream is not usable after ownership is transferred to this adapter.
// Initializing the adapter will fail if the stream is already locked or
// disturbed.
//
// If the adapter is dropped, or canceled while there are pending reads,
// the pending reads will be rejected with the same exception as the cancel.
// Because JavaScript promises are not cancelable, reads that are in progress
// won't be aborted immediately but the results will be ignored when they
// complete and a best-effort will be made to interrupt the read as soon as
// possible. If the stream is already closed, reads will complete immediately
// with 0 bytes read. If the stream errors, reads will reject with the same
// exception.
//
// The minRead contract is enforced. The adapter will attempt to read at
// least minBytes on each read, under the isolate lock. If the stream ends
// before minBytes can be satisfied, the read will complete with whatever
// bytes were available and the adapter will remember that the stream is
// closed.
//
// Concurrent/overlapping reads are not allowed. If a read is already
// pending, further read attempts will be rejected.
//
// While the caller is expected to follow the ReadableStreamSource contract
// and keep the adapter and buffer alive until the read promises resolve,
// there are some protections in place to avoid use-after-free if the caller
// drops the adapter. There's nothing we can do if the caller drops the
// buffer, however, so that is still a hard requirement.
// TODO(safety): This can be made safer by having tryRead take a kj::Array
// as input instead of a raw pointer and size, then having the read return
// the filled in Array after the read completes, but that's a larger refactor.
class ReadableStreamSourceKjAdapter final: public ReadableStreamSource {
 public:
  enum class MinReadPolicy {
    // The read will complete as soon as at least minBytes have been read,
    // even if more bytes are available and the buffer is not full. This
    // may result in more read calls (keeping in mind that each read needs
    // to acquire the isolate lock) but may keep the stream flowing more.
    IMMEDIATE,
    // The read will attempt to fill the entire buffer until either
    // maxBytes, the stream ends, or we determine the buffer is "full enough".
    // This will result in fewer read calls (and thus grabbing the isolate
    // lock less often) but may result in higher latency for each read.
    OPPORTUNISTIC,
  };
  struct Options {
    MinReadPolicy minReadPolicy;
  };

  ReadableStreamSourceKjAdapter(jsg::Lock& js,
      IoContext& ioContext,
      jsg::Ref<ReadableStream> stream,
      Options options = {.minReadPolicy = MinReadPolicy::OPPORTUNISTIC});
  ~ReadableStreamSourceKjAdapter() noexcept(false);

  // Attempts to read at least minBytes and up to maxBytes into the provided
  // buffer. The returned promise resolves with the actual number of bytes read,
  // which may be less than minBytes if the stream is fully consumed.
  //
  // If the stream is already closed, the returned promise resolves
  // immediately with 0. If the stream is canceled or errors, the returned
  // promise rejects with the same exception.
  //
  // minBytes must be less than or equal to maxBytes and greater than zero.
  // If any values outside that range are provided, minBytes will be clamped
  // to the range [1, maxBytes].
  //
  // Per the contact of tryRead, it is the caller's responsibility to ensure
  // that both the buffer and this adapter remain alive until the returned
  // promise resolves! It is also the caller's responsibility to ensure that
  // buffer is at least maxBytes in length. However, there are some protections
  // implemented to avoid use-after-free if the adapter is dropped while a read
  // is in progress.
  //
  // The returned promise will never resolve with more than maxBytes.
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override;

  // Fully consume the stream and write it to the provided WritableStreamSink.
  // If "end" is true, the output stream will be ended once the input
  // stream is fully consumed.
  // Per the contract of pumpTo, it is the caller's responsibility to ensure
  // that both the WritableStreamSink and this adapter remain alive until
  // the returned promise resolves!
  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override;

  // If the stream is still active, tries to get the total length,
  // if known. If the length is not known, the encoding does not
  // match the encoding of the underlying stream, or the stream is closed
  // or errored, returns kj::none.
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override;

  // Cancels the underlying source if it is still active.
  void cancel(kj::Exception reason) override;

  StreamEncoding getPreferredEncoding() override {
    // Our underlying ReadableStream produces non-encoded bytes.
    return StreamEncoding::IDENTITY;
  };

  kj::Maybe<Tee> tryTee(uint64_t limit) override {
    // While ReadableStream in general supports teeing, we aren't going
    // to support it here because of the complexity involved (and we
    // just don't need it).
    return kj::none;
  }

  struct ReadContext;
  KJ_DECLARE_NON_POLYMORPHIC(ReadContext);

 private:
  struct Active;
  KJ_DECLARE_NON_POLYMORPHIC(Active);
  struct Closed {};
  kj::OneOf<kj::Own<Active>, Closed, kj::Exception> state;
  const Options options;
  kj::Rc<WeakRef<ReadableStreamSourceKjAdapter>> selfRef;

  kj::Promise<size_t> tryReadImpl(Active& active, kj::ArrayPtr<kj::byte> buffer, size_t minBytes);
  kj::Promise<void> pumpToImpl(WritableStreamSink& output, bool end);
  static jsg::Promise<kj::Own<ReadContext>> readInternal(
      jsg::Lock& js, kj::Own<ReadContext> context, MinReadPolicy minReadPolicy);
};

}  // namespace workerd::api::streams
