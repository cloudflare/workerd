// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compression.h"

#include "nbytes.h"

#include <workerd/api/system-streams.h>
#include <workerd/io/features.h>
#include <workerd/util/ring-buffer.h>
#include <workerd/util/state-machine.h>

namespace workerd::api {
CompressionAllocator::CompressionAllocator(
    kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
    : externalMemoryTarget(kj::mv(externalMemoryTarget)) {}

void CompressionAllocator::configure(z_stream* stream) {
  stream->zalloc = AllocForZlib;
  stream->zfree = FreeForZlib;
  stream->opaque = this;
}

void* CompressionAllocator::AllocForZlib(void* data, uInt items, uInt size) {
  size_t real_size =
      nbytes::MultiplyWithOverflowCheck(static_cast<size_t>(items), static_cast<size_t>(size));
  return AllocForBrotli(data, real_size);
}

void* CompressionAllocator::AllocForBrotli(void* opaque, size_t size) {
  auto* allocator = static_cast<CompressionAllocator*>(opaque);
  auto data = kj::heapArray<kj::byte>(size);
  auto begin = data.begin();

  allocator->allocations.insert(begin,
      {.data = kj::mv(data),
        .memoryAdjustment = allocator->externalMemoryTarget->getAdjustment(size)});
  return begin;
}

void CompressionAllocator::FreeForZlib(void* opaque, void* pointer) {
  if (KJ_UNLIKELY(pointer == nullptr)) return;
  auto* allocator = static_cast<CompressionAllocator*>(opaque);
  // No need to destroy memoryAdjustment here.
  // Dropping the allocation from the hashmap will defer the adjustment
  // until the isolate lock is held.
  JSG_REQUIRE(allocator->allocations.erase(pointer), Error, "Zlib allocation should exist"_kj);
}

namespace {

class Context {
 public:
  enum class Mode {
    COMPRESS,
    DECOMPRESS,
  };

  enum class ContextFlags {
    NONE,
    STRICT,
  };

  struct Result {
    bool success = false;
    kj::ArrayPtr<const byte> buffer;
  };

  explicit Context(Mode mode,
      kj::StringPtr format,
      ContextFlags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
      : allocator(kj::mv(externalMemoryTarget)),
        mode(mode),
        strictCompression(flags)

  {
    // Configure allocator before any stream operations.
    allocator.configure(&ctx);
    int result = Z_OK;
    switch (mode) {
      case Mode::COMPRESS:
        result = deflateInit2(&ctx, Z_DEFAULT_COMPRESSION, Z_DEFLATED, getWindowBits(format),
            8,  // memLevel = 8 is the default
            Z_DEFAULT_STRATEGY);
        break;
      case Mode::DECOMPRESS:
        result = inflateInit2(&ctx, getWindowBits(format));
        break;
      default:
        KJ_UNREACHABLE;
    }
    JSG_REQUIRE(result == Z_OK, Error, "Failed to initialize compression context."_kj);
  }

  ~Context() noexcept(false) {
    switch (mode) {
      case Mode::COMPRESS:
        deflateEnd(&ctx);
        break;
      case Mode::DECOMPRESS:
        inflateEnd(&ctx);
        break;
    }
  }

  KJ_DISALLOW_COPY_AND_MOVE(Context);

  void setInput(const void* in, size_t size) {
    ctx.next_in = const_cast<byte*>(reinterpret_cast<const byte*>(in));
    ctx.avail_in = size;
  }

  Result pumpOnce(int flush) {
    ctx.next_out = buffer;
    ctx.avail_out = sizeof(buffer);

    int result = Z_OK;

    switch (mode) {
      case Mode::COMPRESS:
        result = deflate(&ctx, flush);
        JSG_REQUIRE(result == Z_OK || result == Z_BUF_ERROR || result == Z_STREAM_END, TypeError,
            "Compression failed.");
        break;
      case Mode::DECOMPRESS:
        result = inflate(&ctx, flush);
        JSG_REQUIRE(result == Z_OK || result == Z_BUF_ERROR || result == Z_STREAM_END, TypeError,
            "Decompression failed.");

        if (strictCompression == ContextFlags::STRICT) {
          // The spec requires that a TypeError is produced if there is trailing data after the end
          // of the compression stream.
          JSG_REQUIRE(!(result == Z_STREAM_END && ctx.avail_in > 0), TypeError,
              "Trailing bytes after end of compressed data");
          // Same applies to closing a stream before the complete decompressed data is available.
          JSG_REQUIRE(
              !(flush == Z_FINISH && result == Z_BUF_ERROR && ctx.avail_out == sizeof(buffer)),
              TypeError, "Called close() on a decompression stream with incomplete data");
        }
        break;
      default:
        KJ_UNREACHABLE;
    }

    return Result{
      .success = result == Z_OK,
      .buffer = kj::arrayPtr(buffer, sizeof(buffer) - ctx.avail_out),
    };
  }

 protected:
  CompressionAllocator allocator;

 private:
  static int getWindowBits(kj::StringPtr format) {
    // We use a windowBits value of 15 combined with the magic value
    // for the compression format type. For gzip, the magic value is
    // 16, so the value returned is 15 + 16. For deflate, the magic
    // value is 15. For raw deflate (i.e. deflate without a zlib header)
    // the negative windowBits value is used, so -15. See the comments for
    // deflateInit2() in zlib.h for details.
    static constexpr auto GZIP = 16;
    static constexpr auto DEFLATE = 15;
    static constexpr auto DEFLATE_RAW = -15;
    if (format == "gzip")
      return DEFLATE + GZIP;
    else if (format == "deflate")
      return DEFLATE;
    else if (format == "deflate-raw")
      return DEFLATE_RAW;
    KJ_UNREACHABLE;
  }

  Mode mode;
  z_stream ctx = {};
  kj::byte buffer[16384];

  // For the eponymous compatibility flag
  ContextFlags strictCompression;
};

// Buffer class based on std::vector that erases data that has been read from it lazily to avoid
// excessive copying when reading a larger amount of buffered data in small chunks. valid_size_ is
// used to track the amount of data that has not been read back yet.
class LazyBuffer {
 public:
  LazyBuffer(): valid_size_(0) {}

  // Return a chunk of data and mark it as invalid. The returned chunk remains valid until data is
  // shifted, cleared or destructor is called. maybeShift() should be called after the returned data
  // has been processed.
  kj::ArrayPtr<byte> take(size_t read_size) {
    KJ_ASSERT(read_size <= valid_size_);
    kj::ArrayPtr<byte> chunk = kj::arrayPtr(&output[output.size() - valid_size_], read_size);
    valid_size_ -= read_size;
    return chunk;
  }

  // Shift the output only if doing so results in reducing vector size by at least 1 KiB and 1/8 of
  // its size to avoid copying for small reads.
  void maybeShift() {
    size_t unusedSpace = output.size() - valid_size_;
    if (unusedSpace >= 1024 && unusedSpace >= (output.size() >> 3)) {
      // Shifting buffer to erase data that has already been read. valid_size_ remains the same.
      memmove(output.begin(), output.begin() + unusedSpace, valid_size_);
      output.truncate(valid_size_);
    }
  }

  void write(kj::ArrayPtr<const byte> chunk) {
    output.addAll(chunk);
    valid_size_ += chunk.size();
  }

  void clear() {
    output.clear();
    valid_size_ = 0;
  }

  // For convenience, provide the size of the valid data that has not been read back yet. This may
  // be smaller than the size of the internal vector, which is not relevant for the stream
  // implementation.
  size_t size() {
    return valid_size_;
  }

  // As with size(), the buffer is considered empty if there is no valid data remaining.
  size_t empty() {
    return valid_size_ == 0;
  }

 private:
  kj::Vector<kj::byte> output;
  size_t valid_size_;
};

// Uncompressed data goes in. Compressed data comes out.
template <Context::Mode mode>
class CompressionStreamImpl: public kj::Refcounted,
                             public kj::AsyncInputStream,
                             public capnp::ExplicitEndOutputStream {
 public:
  explicit CompressionStreamImpl(kj::String format,
      Context::ContextFlags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
      : context(mode, format, flags, kj::mv(externalMemoryTarget)) {
    state.template transitionTo<Open>();
  }

  // WritableStreamSink implementation ---------------------------------------------------

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    requireActive("Write after close");
    context.setInput(buffer.begin(), buffer.size());
    writeInternal(Z_NO_FLUSH);
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    // We check state here so that we catch errors even if pieces is empty.
    requireActive("Write after close");
    for (auto piece: pieces) {
      co_await write(piece);
    }
    co_return;
  }

  kj::Promise<void> end() override {
    // Use transitionFromTo to ensure we're in Open state before ending.
    // This provides a clearer error if end() is called twice.
    auto result = state.template transitionFromTo<Open, Ended>();
    KJ_REQUIRE(result != kj::none, "Stream already ended or errored");
    writeInternal(Z_FINISH);
    co_return;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  void abortWrite(kj::Exception&& reason) override {
    cancelInternal(kj::mv(reason));
  }

  // AsyncInputStream implementation -----------------------------------------------------

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    KJ_ASSERT(minBytes <= maxBytes);
    // Re-throw any stored exception
    KJ_IF_SOME(exception, state.tryGetError()) {
      kj::throwFatalException(kj::cp(exception));
    }
    // If stream has ended normally and no buffered data, return EOF
    if (state.isTerminal() && output.empty()) {
      co_return static_cast<size_t>(0);
    }
    // Active or terminal with data remaining
    co_return co_await tryReadInternal(
        kj::arrayPtr(reinterpret_cast<kj::byte*>(buffer), maxBytes), minBytes);
  }

 private:
  // Helper to check that the stream is still active (Open state).
  // Throws an appropriate error if the stream has ended or errored.
  void requireActive(kj::StringPtr errorMessage) {
    KJ_IF_SOME(exception, state.tryGetError()) {
      kj::throwFatalException(kj::cp(exception));
    }
    // isActive() returns true only if in Open state (the ActiveState)
    JSG_REQUIRE(state.isActive(), Error, errorMessage);
  }

  struct PendingRead {
    kj::ArrayPtr<kj::byte> buffer;
    size_t minBytes = 1;
    size_t filled = 0;
    kj::Own<kj::PromiseFulfiller<size_t>> promise;
  };

  void cancelInternal(kj::Exception reason) {
    output.clear();

    while (!pendingReads.empty()) {
      auto pending = kj::mv(pendingReads.front());
      pendingReads.pop_front();
      if (pending.promise->isWaiting()) {
        pending.promise->reject(kj::cp(reason));
      }
    }

    canceler.cancel(kj::cp(reason));
    // Use forceTransitionTo because cancelInternal may be called when already
    // in an error state (e.g., from writeInternal error handling).
    state.template forceTransitionTo<kj::Exception>(kj::mv(reason));
  }

  kj::Promise<size_t> tryReadInternal(kj::ArrayPtr<kj::byte> dest, size_t minBytes) {
    const auto copyIntoBuffer = [this](kj::ArrayPtr<kj::byte> dest) {
      auto maxBytesToCopy = kj::min(dest.size(), output.size());
      dest.first(maxBytesToCopy).copyFrom(output.take(maxBytesToCopy));
      output.maybeShift();
      return maxBytesToCopy;
    };

    // If the output currently contains >= minBytes, then we'll fulfill
    // the read immediately, removing as many bytes as possible from the
    // output queue.
    // If we reached the end (terminal state), resolve the read immediately
    // as well, since no new data is expected.
    if (output.size() >= minBytes || state.isTerminal()) {
      co_return copyIntoBuffer(dest);
    }

    // Otherwise, create a pending read.
    auto promise = kj::newPromiseAndFulfiller<size_t>();
    auto pendingRead = PendingRead{
      .buffer = dest,
      .minBytes = minBytes,
      .filled = 0,
      .promise = kj::mv(promise.fulfiller),
    };

    // If there are any bytes queued, copy as much as possible into the buffer.
    if (output.size() > 0) {
      pendingRead.filled = copyIntoBuffer(dest);
    }

    pendingReads.push_back(kj::mv(pendingRead));

    co_return co_await canceler.wrap(kj::mv(promise.promise));
  }

  void writeInternal(int flush) {
    // TODO(later): This does not yet implement any backpressure. A caller can keep calling
    // write without reading, which will continue to fill the internal buffer.
    // Either we're finishing (state is Ended) or we must still be active (Open)
    KJ_ASSERT(flush == Z_FINISH || state.isActive());
    Context::Result result;

    while (true) {
      KJ_IF_SOME(exception, kj::runCatchingExceptions([this, flush, &result]() {
        result = context.pumpOnce(flush);
      })) {
        cancelInternal(kj::cp(exception));
        kj::throwFatalException(kj::mv(exception));
      }

      if (result.buffer.size() == 0) {
        if (result.success) {
          // No output produced but input data has been processed based on zlib return code, call
          // pumpOnce again.
          continue;
        }
        maybeFulfillRead();
        return;
      }

      // Output has been produced, copy it to result buffer and continue loop to call pumpOnce
      // again.
      output.write(result.buffer);
    }
    KJ_UNREACHABLE;
  }

  // Fulfill as many pending reads as we can from the output buffer.
  void maybeFulfillRead() {
    // If there are pending reads and data to be read, we'll loop through
    // the pending reads and fulfill them as much as possible.
    while (!pendingReads.empty() && output.size() > 0) {
      auto& pending = pendingReads.front();

      if (!pending.promise->isWaiting()) {
        // The pending read was canceled!
        // Importantly, the pending.buffer is no longer valid here so we definitely want to
        // make sure we don't try to write anything to it!

        // If the pending read was already partially fulfilled, then we have a problem!
        // We can't just cancel and continue because the partially read data will be lost
        // so we need to report an error here and error the stream.
        if (pending.filled > 0) {
          auto ex = JSG_KJ_EXCEPTION(FAILED, Error, "A partially fulfilled read was canceled.");
          cancelInternal(kj::cp(ex));
          kj::throwFatalException(kj::mv(ex));
        }

        auto ex = JSG_KJ_EXCEPTION(FAILED, Error, "The pending read was canceled.");
        cancelInternal(kj::cp(ex));
        kj::throwFatalException(kj::mv(ex));
      }

      // The pending read is still viable so determine how much we can copy in.
      auto amountToCopy = kj::min(pending.buffer.size() - pending.filled, output.size());
      kj::ArrayPtr<byte> chunk = output.take(amountToCopy);
      pending.buffer.slice(pending.filled, pending.filled + amountToCopy).copyFrom(chunk);
      pending.filled += amountToCopy;
      output.maybeShift();

      // If we've met the minimum bytes requirement for the pending read, fulfill
      // the read promise.
      if (pending.filled >= pending.minBytes) {
        auto p = kj::mv(pending);
        pendingReads.pop_front();
        p.promise->fulfill(kj::mv(p.filled));
        continue;
      }

      // If we reached this point in the loop, remaining must be 0 so that we
      // don't keep iterating through on the same pending read.
      KJ_ASSERT(output.empty());
    }

    if (state.template is<Ended>() && !pendingReads.empty()) {
      // We are ended and we have pending reads. Because of the loop above,
      // one of either pendingReads or output must be empty, so if we got this
      // far, output.empty() must be true. Let's check.
      KJ_ASSERT(output.empty());
      // We need to flush any remaining reads.
      while (!pendingReads.empty()) {
        auto pending = kj::mv(pendingReads.front());
        pendingReads.pop_front();
        if (pending.promise->isWaiting()) {
          // Fulfill the pending read promise only if it hasn't already been canceled.
          pending.promise->fulfill(kj::mv(pending.filled));
        }
      }
    }
  }

  struct Ended {
    static constexpr kj::StringPtr NAME = "ended"_kj;
  };
  struct Open {
    static constexpr kj::StringPtr NAME = "open"_kj;
  };

  // State machine for tracking compression stream lifecycle:
  //   Open -> Ended (normal close via end())
  //   Open -> kj::Exception (error via abortWrite())
  // Both Ended and kj::Exception are terminal states.
  StateMachine<TerminalStates<Ended, kj::Exception>,
      ErrorState<kj::Exception>,
      ActiveState<Open>,
      Open,
      Ended,
      kj::Exception>
      state;
  Context context;

  kj::Canceler canceler;
  LazyBuffer output;
  RingBuffer<PendingRead, 8> pendingReads;
};

// Adapter to bridge CompressionStreamImpl (which implements AsyncInputStream and
// ExplicitEndOutputStream) to the ReadableStreamSource/WritableStreamSink interfaces.
// TODO(soon): This class is intended to be replaced by the new ReadableSource/WritableSink
// interfaces once fully implemented. We will need an adapter that knows how to handle both
// sides of the stream once fully implemented. The current implementation in system-streams.c++
// implements separate adapters for each side that are not aware of each other, making it
// unsuitable for this specific case.
template <Context::Mode mode>
class CompressionStreamAdapter final: public kj::Refcounted,
                                      public ReadableStreamSource,
                                      public WritableStreamSink {
 public:
  explicit CompressionStreamAdapter(kj::Rc<CompressionStreamImpl<mode>> impl)
      : impl(kj::mv(impl)),
        ioContext(IoContext::current()) {}

  // ReadableStreamSource implementation
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return impl->tryRead(buffer, minBytes, maxBytes).attach(ioContext.registerPendingEvent());
  }

  void cancel(kj::Exception reason) override {
    // AsyncInputStream doesn't have cancel, but we can abort the write side
    impl->abortWrite(kj::mv(reason));
  }

  // WritableStreamSink implementation
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return impl->write(buffer).attach(ioContext.registerPendingEvent());
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return impl->write(pieces).attach(ioContext.registerPendingEvent());
  }

  kj::Promise<void> end() override {
    return impl->end().attach(ioContext.registerPendingEvent());
  }

  void abort(kj::Exception reason) override {
    impl->abortWrite(kj::mv(reason));
  }

 private:
  kj::Rc<CompressionStreamImpl<mode>> impl;
  IoContext& ioContext;
};

}  // namespace

jsg::Ref<CompressionStream> CompressionStream::constructor(jsg::Lock& js, kj::String format) {
  JSG_REQUIRE(format == "deflate" || format == "gzip" || format == "deflate-raw", TypeError,
      "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.");

  auto impl = kj::rc<CompressionStreamImpl<Context::Mode::COMPRESS>>(
      kj::mv(format), Context::ContextFlags::NONE, js.getExternalMemoryTarget());

  auto& ioContext = IoContext::current();

  // Create a single adapter that implements both readable and writable sides
  auto adapter = kj::refcounted<CompressionStreamAdapter<Context::Mode::COMPRESS>>(kj::mv(impl));
  auto readableSide = kj::addRef(*adapter);
  auto writableSide = kj::mv(adapter);

  return js.alloc<CompressionStream>(js.alloc<ReadableStream>(ioContext, kj::mv(readableSide)),
      js.alloc<WritableStream>(ioContext, kj::mv(writableSide),
          ioContext.getMetrics().tryCreateWritableByteStreamObserver()));
}

jsg::Ref<DecompressionStream> DecompressionStream::constructor(jsg::Lock& js, kj::String format) {
  JSG_REQUIRE(format == "deflate" || format == "gzip" || format == "deflate-raw", TypeError,
      "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.");

  auto impl = kj::rc<CompressionStreamImpl<Context::Mode::DECOMPRESS>>(kj::mv(format),
      FeatureFlags::get(js).getStrictCompression() ? Context::ContextFlags::STRICT
                                                   : Context::ContextFlags::NONE,
      js.getExternalMemoryTarget());

  auto& ioContext = IoContext::current();

  // Create a single adapter that implements both readable and writable sides
  auto adapter = kj::refcounted<CompressionStreamAdapter<Context::Mode::DECOMPRESS>>(kj::mv(impl));
  auto readableSide = kj::addRef(*adapter);
  auto writableSide = kj::mv(adapter);

  return js.alloc<DecompressionStream>(js.alloc<ReadableStream>(ioContext, kj::mv(readableSide)),
      js.alloc<WritableStream>(ioContext, kj::mv(writableSide),
          ioContext.getMetrics().tryCreateWritableByteStreamObserver()));
}

}  // namespace workerd::api
