#pragma once

#include <workerd/io/worker-interface.capnp.h>

#include <kj/debug.h>

#include <atomic>

namespace kj {
class AsyncOutputStream;
}

namespace workerd {

class IoContext;

namespace api::streams {

// A WritableSink is primarily intended to serve as a bridge between kj::AsyncOutputStream
// and the WritableStream API. However, it can also be used directly by KJ-space code. While
// WritableSink should probably have been a more JS-friendly API, it's a bit too late
// to change that now. Use the WritableSinkJsAdapter in the writable-sink-adapter.h file
// to wrap a WritableSink for use from JavaScript.
//
// Not all WritableSink implementations will be explicitly backed by a KJ stream;
// some might be test implementations that discard data or accumulate it in memory, for
// instance.
//
// A WritableSink must be treated like a KJ I/O object. Instances that are held
// by any JS-heap objects must be held by an IoOwn.
//
// The sink permits only one write() or end() operation to be pending at a time. If
// a second write() or end() is attempted while one is already pending, the promise
// returned by the second call will be rejected with a jsg::Error. This is to
// match the behavior of the kj::AsyncOutputStream interface.
//
// If the sink is aborted or dropped, any pending write() or end() operations will be
// canceled.
class WritableSink {
 public:
  // Write the given buffer to the stream, returning a promise that resolves when the write
  // completes.
  virtual kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) KJ_WARN_UNUSED_RESULT = 0;

  // Write the given pieces to the stream, returning a promise that resolves when the write
  // completes.
  virtual kj::Promise<void> write(
      kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) KJ_WARN_UNUSED_RESULT = 0;

  // Ends the stream, transitioning it to the closed state. After this, no further writes
  // will be accepted.
  virtual kj::Promise<void> end() KJ_WARN_UNUSED_RESULT = 0;

  // Aborts the stream, transitioning it to the errored state. After this, no further writes
  // will be accepted.
  virtual void abort(kj::Exception reason) = 0;

  // Tells the sink that it is no longer to be responsible for encoding in the correct format.
  // Instead, the caller takes responsibility. The expected encoding is returned; the caller
  // promises that all future writes will use this encoding.
  virtual rpc::StreamEncoding disownEncodingResponsibility() = 0;

  // Return the encoding that this sink is using.
  virtual rpc::StreamEncoding getEncoding() = 0;
};

// Utility base class for WritableSink wrappers that delegate all
// operations to an inner WritableSink while selectively overriding
// some operations.
class WritableSinkWrapper: public WritableSink {
 public:
  virtual ~WritableSinkWrapper() noexcept(false) {
    canceler.cancel(KJ_EXCEPTION(DISCONNECTED, "Dropped"));
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return getInner().write(buffer);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return getInner().write(pieces);
  }

  kj::Promise<void> end() override {
    return getInner().end();
  }

  void abort(kj::Exception reason) override {
    getInner().abort(kj::mv(reason));
  }

  rpc::StreamEncoding disownEncodingResponsibility() override {
    return getInner().disownEncodingResponsibility();
  }

  rpc::StreamEncoding getEncoding() override {
    return getInner().getEncoding();
  }

  // Releases ownership of the inner WritableSink. After calling this,
  // this instance is no longer usable.
  kj::Own<WritableSink> release() {
    auto ret = kj::mv(KJ_ASSERT_NONNULL(inner));
    inner = kj::none;
    canceler.cancel(KJ_EXCEPTION(DISCONNECTED, "Released"));
    return kj::mv(ret);
  }

 protected:
  WritableSinkWrapper(kj::Own<WritableSink> inner): inner(kj::mv(inner)) {}
  KJ_DISALLOW_COPY_AND_MOVE(WritableSinkWrapper);

  WritableSink& getInner() {
    return *KJ_ASSERT_NONNULL(inner);
  }

 private:
  kj::Canceler canceler;
  kj::Maybe<kj::Own<WritableSink>> inner;
};

// Creates a WritableSink that wraps a kj::AsyncOutputStream.
kj::Own<WritableSink> newWritableSink(kj::Own<kj::AsyncOutputStream> inner);

// Creates a WritableSink that is in the closed state.
kj::Own<WritableSink> newClosedWritableSink();

// Creates a WritableSink that is permanently in the errored state.
kj::Own<WritableSink> newErroredWritableSink(kj::Exception reason);

// Creates a WritableSink that discards all data written to it.
kj::Own<WritableSink> newNullWritableSink();

// Creates a WritableSink that encodes data written to it.
kj::Own<WritableSink> newEncodedWritableSink(
    rpc::StreamEncoding encoding, kj::Own<kj::AsyncOutputStream> inner);

// Wraps a WritableSink such that each write()/end() call on the returned sink will
// register as a pending event on the IoContext.
kj::Own<WritableSink> newIoContextWrappedWritableSink(
    IoContext& ioContext, kj::Own<WritableSink> inner);

// A refcounted byte counter that can be shared between a stream wrapper and
// the code that needs to read the final byte count after streaming completes.
// Using atomic for thread safety since proxyTask may run on different thread.
struct ByteCounter: public kj::Refcounted {
  std::atomic<uint64_t> bytesWritten{0};

  void add(size_t bytes) {
    bytesWritten.fetch_add(bytes, std::memory_order_relaxed);
  }

  uint64_t get() const {
    return bytesWritten.load(std::memory_order_relaxed);
  }
};

// A WritableSink wrapper that counts the total bytes written to the underlying sink.
// This is used to track actual response body sizes for trace events.
class ByteCountingWritableSink final: public WritableSinkWrapper {
 public:
  ByteCountingWritableSink(kj::Own<WritableSink> inner, kj::Own<ByteCounter> counter)
      : WritableSinkWrapper(kj::mv(inner)),
        counter(kj::mv(counter)) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    counter->add(buffer.size());
    return WritableSinkWrapper::write(buffer);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (const auto& piece: pieces) {
      counter->add(piece.size());
    }
    return WritableSinkWrapper::write(pieces);
  }

 private:
  kj::Own<ByteCounter> counter;
};

}  // namespace api::streams
}  // namespace workerd
