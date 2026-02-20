#pragma once

#include <workerd/io/worker-interface.capnp.h>
#include <workerd/util/strong-bool.h>

#include <kj/debug.h>

namespace kj {
class AsyncInputStream;
}

namespace workerd {

class IoContext;
namespace api {
template <typename T>
struct DeferredProxy;
class ReadableStreamSource;
}  // namespace api

namespace jsg {
class Lock;
}  // namespace jsg

namespace api::streams {

class WritableSink;

WD_STRONG_BOOL(EndAfterPump);

// A ReadableSource is primarily intended to serve as a bridge between kj::AsyncInputStream
// and the ReadableStream API. However, it can also be used directly by KJ-space code that needs
// deferred proxying. While ReadableSource should probably have been a more JS-friendly
// API, it's a bit too late to change that now. Use the ReadableSourceJsAdapter in the
// readable-source-adapter.h file to wrap a ReadableSource for use from JavaScript.
//
// A ReadableSource must be treated like a KJ I/O object. Instances that are held
// by any JS-heap objects must be held by an IoOwn.
//
// If the ReadableSource is canceled or dropped, all pending read() reads will be
// canceled.
//
// Only one read() may be pending at a time. Attempting to initiate a second read()
// while one is already pending will result in a rejected promise.
//
// Calling pumpTo initiates a sequence of read() calls until the stream is fully consumed.
// Ownership of the underlying AsyncInputStream is transferred to the pumpTo operation and
// the ReadableSource is put into a closed state. After calling pumpTo, no further
// read() calls may be made directly on the ReadableSource. Dropping the returned
// promise before it resolves will cancel the pump operation.
//
// It is **NOT** intended that you should implement this interface for general use.
// It is only intended to be implemented by specific classes within workerd for the
// purpose of bridging between kj/js streams. Streams that operate at the kj level
// should implement the kj::Async*Stream interfaces, and streams that operate at
// the JS level should implement to the UnderlyingSource interface. This is a
// departure from what we've done previously but as part of the effort to simplify
// the streams code, the goal is to reduce the number of different stream interfaces
// that we implement to.
class ReadableSource {
 public:
  // Read into the given buffer, returning a promise that resolves to the number of bytes read.
  // The maximum number of bytes that will be read is the size of the buffer. The minimum number
  // of bytes that will be read is minBytes. If at least minBytes cannot be read, the promise
  // will be resolved with the number of bytes read and the stream will be closed.
  virtual kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes = 1) = 0;

  // If `end` is true, then `output.end()` will be called after pumping. Note that it's especially
  // important to take advantage of this when using deferred proxying since calling `end()`
  // directly might attempt to use the `IoContext` to call `registerPendingEvent()`.
  // If the pump fails, the ReadableSource will be left in an errored state. The
  // default implementation uses read() to read chunks of data and write them to the output
  // using a 16KB buffer.
  //
  // Per the contract of pumpTo(), it is the caller's responsibility to ensure that both
  // the WritableStreamSink and this ReadableSource remain alive until the returned
  // promise resolves!
  //
  // It is the caller's responsibility to ensure that WritableStreamSink and this
  // ReadableSource remain alive until the wrapped deferred proxy task resolves.
  // The default implementation does arrange to make it safe to drop the source once
  // the pump begins but that's only precautionary/defensive. It's still better/safer
  // for the caller to keep the source alive until the pump completes.
  virtual kj::Promise<DeferredProxy<void>> pumpTo(
      WritableSink& output, EndAfterPump end = EndAfterPump::YES) = 0;

  // If the stream is still active, and the encoding matches an encoding that the stream
  // can provide, gets the total length, if known. If the length is not known, or the
  // encoding does not match the encoding of the underlying stream, or the stream is closed
  // or errored, returns kj::none.
  virtual kj::Maybe<size_t> tryGetLength(rpc::StreamEncoding encoding) = 0;

  // Fully consume the stream and return all of its data as a byte array. The limit
  // parameter is the maximum number of bytes to read. If the stream contains more
  // than this number of bytes, the promise will reject with an exception.
  virtual kj::Promise<kj::Array<const kj::byte>> readAllBytes(size_t limit) = 0;

  // Fully consume the stream and return all of its data as a string. The limit
  // parameter is the maximum number of bytes to read. If the stream contains more
  // than this number of bytes, the promise will reject with an exception.
  virtual kj::Promise<kj::String> readAllText(size_t limit) = 0;

  // Cancels the underlying source if it is still active. Must put the stream into an
  // errored state. After calling this, all pending and future reads should fail.
  // Dropping the ReadableSource without calling cancel() first should trigger
  // cancel() with a generic exception.
  virtual void cancel(kj::Exception reason) = 0;

  struct Tee {
    kj::Own<ReadableSource> branch1;
    kj::Own<ReadableSource> branch2;
  };

  // Tees the stream into two branches. The returned Tee contains two new ReadableSource
  // instances that will each receive the same data. Once this is called, this instance is no
  // longer usable and will behave as if it has been closed.
  // The limit parameter specifies the maximum buffer size to use when teeing.
  virtual Tee tee(size_t limit) = 0;

  // Gets the encoding of the stream.
  virtual rpc::StreamEncoding getEncoding() = 0;
};

// Utility base class for ReadableSource wrappers that delegate all
// operations to an inner ReadableSource while selectively overriding
// some operations.
class ReadableSourceWrapper: public ReadableSource {
 public:
  KJ_DISALLOW_COPY_AND_MOVE(ReadableSourceWrapper);
  virtual ~ReadableSourceWrapper() noexcept(false) = default;

  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes = 1) override {
    return getInner().read(buffer, minBytes);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(
      WritableSink& output, EndAfterPump end = EndAfterPump::YES) override {
    return getInner().pumpTo(output, end);
  }

  kj::Promise<kj::Array<const kj::byte>> readAllBytes(size_t limit) override {
    return getInner().readAllBytes(limit);
  }

  kj::Promise<kj::String> readAllText(size_t limit) override {
    return getInner().readAllText(limit);
  }

  kj::Maybe<size_t> tryGetLength(rpc::StreamEncoding encoding) override {
    return getInner().tryGetLength(encoding);
  }

  void cancel(kj::Exception reason) override {
    getInner().cancel(kj::mv(reason));
  }

  Tee tee(size_t limit) override {
    return getInner().tee(limit);
  }

  rpc::StreamEncoding getEncoding() override {
    return getInner().getEncoding();
  }

  // Releases ownership of the inner ReadableSource. After calling this,
  // this wrapper becomes unusable.
  kj::Own<ReadableSource> release() {
    auto ret = kj::mv(KJ_ASSERT_NONNULL(inner));
    inner = kj::none;
    return kj::mv(ret);
  }

 protected:
  ReadableSourceWrapper(kj::Own<ReadableSource> inner): inner(kj::mv(inner)) {}

  ReadableSource& getInner() {
    return *KJ_ASSERT_NONNULL(inner);
  }

 private:
  kj::Maybe<kj::Own<ReadableSource>> inner;
};

// Creates a ReadableSource that wraps the given kj::AsyncInputStream.
kj::Own<ReadableSource> newReadableSource(kj::Own<kj::AsyncInputStream> inner);

// Creates a ReadableSource that is already in the errored state.
kj::Own<ReadableSource> newErroredReadableSource(kj::Exception exception);

// Creates a ReadableSource that is already closed and will produce no data.
kj::Own<ReadableSource> newClosedReadableSource();

// Creates a ReadableSource that produces the given bytes and then closes.
// The backing object, if any, is held alive until the stream is closed or canceled.
// If the backing object is not provided, the bytes are copied.
kj::Own<ReadableSource> newReadableSourceFromBytes(
    kj::ArrayPtr<const kj::byte> bytes, kj::Maybe<kj::Own<void>> backing = kj::none);

// Creates a ReadableSource that wraps the given source and prevents deferred proxying.
kj::Own<ReadableSource> newIoContextWrappedReadableSource(
    IoContext& ioctx, kj::Own<ReadableSource> inner);

// Creates a ReadableSource that calls the given producer function to produce data
// on each read (useful primarily for testing).
kj::Own<ReadableSource> newReadableSourceFromProducer(
    kj::Function<kj::Promise<size_t>(kj::ArrayPtr<kj::byte>, size_t)> producer,
    kj::Maybe<uint64_t> expectedLength = kj::none);

// Creates a ReadableSource that decodes the given stream according to the given encoding.
kj::Own<ReadableSource> newEncodedReadableSource(
    rpc::StreamEncoding encoding, kj::Own<kj::AsyncInputStream> inner);

// Wraps a kj::AsyncInputStream returned from a tee() call to ensure that it translates
// errors into equivalent JS exceptions. Typically this is used when customizing tee() on
// a ReadableSource implementation.
kj::Own<kj::AsyncInputStream> wrapTeeBranch(kj::Own<kj::AsyncInputStream> branch);

// A ReadableStreamSource backed by in-memory data. Unlike newSystemStream() wrapping a
// newMemoryInputStream(), this implementation does NOT support deferred proxying. This is
// important when the backing memory has V8 heap provenance (e.g., jsg::BackingStore, Blob data,
// kj::Array<kj::byte> with a v8::BackingStore attached, etc)
// since the memory could be freed by GC after the IoContext completes.
//
// The `backing` parameter keeps the underlying memory alive for the lifetime of the stream.
// If not provided, the bytes are copied.
//
// TODO(soon): Update to implement ReadableSource instead of ReadableStreamSource.
// For now this is a ReadableStreamSource for compat with existing code. Once internal.h/c++
// is updated to use ReadableSource, we will change this also.
//
// TODO(cleanup): It would be nice to eventually have some sort of stronger guarantee when
// deferred proxying can or cannot be used with a stream. Right now it's a bit ad hoc and
// error-prone. It requires the stream impl to keep track of whether it can be deferred-proxied
// or not, but in this case, that may be entirely opaque behind the details of the backing memory
// as is the case with kj::Array<kj::byte> instances that come from the type wrapper system.
kj::Own<ReadableStreamSource> newMemorySource(
    kj::ArrayPtr<const kj::byte> bytes, kj::Maybe<kj::Own<void>> backing = kj::none);

}  // namespace api::streams
}  // namespace workerd
