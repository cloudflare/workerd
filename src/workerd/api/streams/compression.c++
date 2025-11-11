// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compression.h"

#include "nbytes.h"

#include <workerd/api/system-streams.h>
#include <workerd/io/features.h>
#include <workerd/util/ring-buffer.h>

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
class CompressionStreamImpl final: public kj::Refcounted,
                                   public capnp::ExplicitEndOutputStream,
                                   public kj::AsyncInputStream {
 public:
  explicit CompressionStreamImpl(kj::StringPtr format,
      Context::ContextFlags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
      : context(mode, format, flags, kj::mv(externalMemoryTarget)) {}
  ~CompressionStreamImpl() noexcept(false) {
    canceler.cancel(KJ_EXCEPTION(DISCONNECTED, "Compression stream closed"));
  }

  // WritableStreamSink implementation ---------------------------------------------------

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ended, Ended) {
        kj::throwFatalException(JSG_KJ_EXCEPTION(FAILED, Error, "Write after close."));
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        kj::throwFatalException(kj::cp(exception));
      }
      KJ_CASE_ONEOF(open, Open) {
        context.setInput(buffer.begin(), buffer.size());
        co_return writeInternal(Z_NO_FLUSH);
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    // We check for Ended, Exception here so that we catch
    // these even if pieces is empty.
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ended, Ended) {
        JSG_FAIL_REQUIRE(Error, "Write after close");
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        kj::throwFatalException(kj::cp(exception));
      }
      KJ_CASE_ONEOF(open, Open) {
        for (auto& piece: pieces) {
          co_await write(piece);
        }
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> end() override {
    state = Ended();
    co_return writeInternal(Z_FINISH);
  }

  void abortWrite(kj::Exception&& reason) override {
    cancelInternal(kj::cp(reason));
  }

  kj::Promise<void> whenWriteDisconnected() override {
    // We don't actually implement whenWriteDisconnected().
    return kj::NEVER_DONE;
  }

  // ReadableStreamSource implementation -------------------------------------------------

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    KJ_ASSERT(minBytes <= maxBytes);
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ended, Ended) {
        // There might still be data in the output buffer remaining to read.
        if (output.empty()) co_return static_cast<size_t>(0);
        co_return co_await tryReadInternal(
            kj::arrayPtr(reinterpret_cast<kj::byte*>(buffer), maxBytes), minBytes);
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        kj::throwFatalException(kj::cp(exception));
      }
      KJ_CASE_ONEOF(open, Open) {
        co_return co_await tryReadInternal(
            kj::arrayPtr(reinterpret_cast<kj::byte*>(buffer), maxBytes), minBytes);
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  struct PendingRead {
    kj::ArrayPtr<kj::byte> buffer;
    size_t minBytes = 1;
    size_t read = 0;
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
    state = kj::mv(reason);
  }

  // Consume up to dest.size() bytes from the output buffer and return the number of bytes consumed.
  size_t consumeOutput(kj::ArrayPtr<kj::byte> dest) {
    size_t toCopy = kj::min(dest.size(), output.size());
    dest.first(toCopy).copyFrom(output.take(toCopy));
    output.maybeShift();
    return toCopy;
  }

  kj::Promise<size_t> tryReadInternal(kj::ArrayPtr<kj::byte> dest, size_t minBytes) {
    // If the output currently contains >= minBytes, then we'll fulfill
    // the read immediately, removing as many bytes as possible from the
    // output queue.
    // If we reached the end, resolve the read immediately as well, since no
    // new data is expected.
    if (output.size() >= minBytes || state.template is<Ended>()) {
      co_return consumeOutput(dest);
    }

    // Otherwise, create a pending read.
    auto promise = kj::newPromiseAndFulfiller<size_t>();
    auto pendingRead = PendingRead{
      .buffer = dest,
      .minBytes = minBytes,
      .read = 0,
      .promise = kj::mv(promise.fulfiller),
    };

    // If there are any bytes queued, copy as much as possible into the buffer.
    if (output.size() > 0) {
      size_t copied = consumeOutput(pendingRead.buffer);
      pendingRead.buffer = pendingRead.buffer.slice(copied);
      pendingRead.read += copied;
      // At this point, we may have partially fulfilled the read but we should still have more.
      KJ_ASSERT(pendingRead.buffer.size() > 0);
      KJ_ASSERT(pendingRead.read < minBytes);
    }

    pendingReads.push_back(kj::mv(pendingRead));

    co_return co_await canceler.wrap(kj::mv(promise.promise));
  }

  void writeInternal(int flush) {
    // TODO(later): This does not yet implement any backpressure. A caller can keep calling
    // write without reading, which will continue to fill the internal buffer.
    KJ_ASSERT(flush == Z_FINISH || state.template is<Open>());
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
        return maybeFulfillRead();
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
        auto ex = JSG_KJ_EXCEPTION(FAILED, Error, "The pending read was canceled.");
        cancelInternal(kj::cp(ex));
        kj::throwFatalException(kj::mv(ex));
      }

      size_t copied = consumeOutput(pending.buffer);
      pending.buffer = pending.buffer.slice(copied);
      pending.read += copied;

      // If we've met the minimum bytes requirement for the pending read, fulfill the read promise.
      if (pending.read >= pending.minBytes) {
        pending.promise->fulfill(kj::mv(pending.read));
        auto free_me = kj::mv(pending);
        pendingReads.pop_front();
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
          pending.promise->fulfill(kj::mv(pending.read));
        }
      }
    }
  }

  struct Ended {};
  struct Open {};

  kj::OneOf<Open, Ended, kj::Exception> state = Open();
  Context context;
  kj::Canceler canceler;
  LazyBuffer output;
  RingBuffer<PendingRead, 8> pendingReads;
};

template <typename Type, Context::Mode mode>
jsg::Ref<Type> constructorImpl(jsg::Lock& js, kj::StringPtr format) {
  JSG_REQUIRE(format == "deflate" || format == "gzip" || format == "deflate-raw", TypeError,
      "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.");

  auto readableSide = kj::rc<CompressionStreamImpl<mode>>(format,
      FeatureFlags::get(js).getStrictCompression() ? Context::ContextFlags::STRICT
                                                   : Context::ContextFlags::NONE,
      js.getExternalMemoryTarget());
  auto writableSide = readableSide.addRef();

  kj::Own<kj::AsyncInputStream> in = kj::mv(readableSide).toOwn();
  kj::Own<capnp::ExplicitEndOutputStream> out = kj::mv(writableSide).toOwn();

  auto& ioContext = IoContext::current();
  auto readable = newSystemStream(kj::mv(in), StreamEncoding::IDENTITY, ioContext);
  auto writable = newSystemStream(kj::mv(out), StreamEncoding::IDENTITY, ioContext);

  return js.alloc<Type>(js.alloc<ReadableStream>(ioContext, kj::mv(readable)),
      js.alloc<WritableStream>(ioContext, kj::mv(writable),
          ioContext.getMetrics().tryCreateWritableByteStreamObserver()));
}
}  // namespace

jsg::Ref<CompressionStream> CompressionStream::constructor(jsg::Lock& js, kj::String format) {
  return constructorImpl<CompressionStream, Context::Mode::COMPRESS>(js, format);
}

jsg::Ref<DecompressionStream> DecompressionStream::constructor(jsg::Lock& js, kj::String format) {
  return constructorImpl<DecompressionStream, Context::Mode::DECOMPRESS>(js, format);
}

}  // namespace workerd::api
