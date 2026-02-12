// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compression.h"

#include "nbytes.h"

#include <workerd/api/system-streams.h>
#include <workerd/io/features.h>
#include <workerd/util/autogate.h>
#include <workerd/util/ring-buffer.h>
#include <workerd/util/state-machine.h>

#include <brotli/decode.h>
#include <brotli/encode.h>

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

enum class Format {
  GZIP,
  DEFLATE,
  DEFLATE_RAW,
  BROTLI,
};

static Format parseFormat(kj::StringPtr format) {
  if (format == "gzip") return Format::GZIP;
  if (format == "deflate") return Format::DEFLATE;
  if (format == "deflate-raw") return Format::DEFLATE_RAW;
  if (format == "brotli") return Format::BROTLI;
  KJ_UNREACHABLE;
}

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
        strictCompression(flags),
        format(parseFormat(format))

  {
    if (this->format == Format::BROTLI) {
      initBrotli();
      return;
    }

    // Configure allocator before any stream operations.
    allocator.configure(&ctx);
    int result = Z_OK;
    switch (mode) {
      case Mode::COMPRESS:
        result = deflateInit2(&ctx, Z_DEFAULT_COMPRESSION, Z_DEFLATED, getWindowBits(this->format),
            8,  // memLevel = 8 is the default
            Z_DEFAULT_STRATEGY);
        break;
      case Mode::DECOMPRESS:
        result = inflateInit2(&ctx, getWindowBits(this->format));
        break;
      default:
        KJ_UNREACHABLE;
    }
    JSG_REQUIRE(result == Z_OK, Error, "Failed to initialize compression context."_kj);
  }

  ~Context() noexcept(false) {
    if (format == Format::BROTLI) {
      switch (mode) {
        case Mode::COMPRESS:
          if (brotliEncoderState != nullptr) {
            BrotliEncoderDestroyInstance(brotliEncoderState);
          }
          break;
        case Mode::DECOMPRESS:
          if (brotliDecoderState != nullptr) {
            BrotliDecoderDestroyInstance(brotliDecoderState);
          }
          break;
      }
      return;
    }
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
    if (format == Format::BROTLI) {
      brotliNextIn = reinterpret_cast<const uint8_t*>(in);
      brotliAvailIn = size;
      return;
    }
    ctx.next_in = const_cast<byte*>(reinterpret_cast<const byte*>(in));
    ctx.avail_in = size;
  }

  Result pumpOnce(int flush) {
    if (format == Format::BROTLI) {
      return pumpBrotliOnce(flush);
    }
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

  bool hasTrailingError() const {
    return brotliTrailingError;
  }

 protected:
  CompressionAllocator allocator;

 private:
  void initBrotli() {
    if (mode == Mode::COMPRESS) {
      auto* instance = BrotliEncoderCreateInstance(
          CompressionAllocator::AllocForBrotli, CompressionAllocator::FreeForZlib, &allocator);
      JSG_REQUIRE(instance != nullptr, Error, "Failed to initialize compression context."_kj);
      brotliEncoderState = instance;
      return;
    }

    auto* instance = BrotliDecoderCreateInstance(
        CompressionAllocator::AllocForBrotli, CompressionAllocator::FreeForZlib, &allocator);
    JSG_REQUIRE(instance != nullptr, Error, "Failed to initialize compression context."_kj);
    brotliDecoderState = instance;
  }

  Result pumpBrotliOnce(int flush) {
    uint8_t* nextOut = buffer;
    size_t availOut = sizeof(buffer);

    if (mode == Mode::COMPRESS) {
      auto op = flush == Z_FINISH ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
      auto ok = BrotliEncoderCompressStream(
          brotliEncoderState, op, &brotliAvailIn, &brotliNextIn, &availOut, &nextOut, nullptr);
      JSG_REQUIRE(ok == BROTLI_TRUE, TypeError, "Compression failed.");

      bool shouldContinue = brotliAvailIn > 0 || BrotliEncoderHasMoreOutput(brotliEncoderState);
      if (op == BROTLI_OPERATION_FINISH && !BrotliEncoderIsFinished(brotliEncoderState)) {
        shouldContinue = true;
      }

      return Result{
        .success = shouldContinue,
        .buffer = kj::arrayPtr(buffer, sizeof(buffer) - availOut),
      };
    }

    auto result = BrotliDecoderDecompressStream(
        brotliDecoderState, &brotliAvailIn, &brotliNextIn, &availOut, &nextOut, nullptr);
    JSG_REQUIRE(result != BROTLI_DECODER_RESULT_ERROR, TypeError, "Decompression failed.");

    if (strictCompression == ContextFlags::STRICT) {
      // Track trailing data so we can surface the error after buffered output drains.
      if (BrotliDecoderIsFinished(brotliDecoderState) && brotliAvailIn > 0) {
        brotliTrailingError = true;
      }
      if (flush == Z_FINISH && result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT &&
          availOut == sizeof(buffer)) {
        JSG_FAIL_REQUIRE(
            TypeError, "Called close() on a decompression stream with incomplete data");
      }
    }

    bool shouldContinue = result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT ||
        BrotliDecoderHasMoreOutput(brotliDecoderState);

    return Result{
      .success = shouldContinue,
      .buffer = kj::arrayPtr(buffer, sizeof(buffer) - availOut),
    };
  }

  static int getWindowBits(Format format) {
    // We use a windowBits value of 15 combined with the magic value
    // for the compression format type. For gzip, the magic value is
    // 16, so the value returned is 15 + 16. For deflate, the magic
    // value is 15. For raw deflate (i.e. deflate without a zlib header)
    // the negative windowBits value is used, so -15. See the comments for
    // deflateInit2() in zlib.h for details.
    static constexpr auto GZIP = 16;
    static constexpr auto DEFLATE = 15;
    static constexpr auto DEFLATE_RAW = -15;
    switch (format) {
      case Format::GZIP:
        return DEFLATE + GZIP;
      case Format::DEFLATE:
        return DEFLATE;
      case Format::DEFLATE_RAW:
        return DEFLATE_RAW;
      case Format::BROTLI:
        KJ_UNREACHABLE;
    }
    KJ_UNREACHABLE;
  }

  Mode mode;
  z_stream ctx = {};
  kj::byte buffer[16384];

  // For the eponymous compatibility flag
  ContextFlags strictCompression;
  Format format;
  const uint8_t* brotliNextIn = nullptr;
  size_t brotliAvailIn = 0;
  // Brotli state structs are opaque, so kj::Own would require complete types.
  BrotliEncoderState* brotliEncoderState = nullptr;
  BrotliDecoderState* brotliDecoderState = nullptr;
  // Defer reporting of trailing brotli bytes until output is drained.
  bool brotliTrailingError = false;
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

// Because we have to use an autogate to switch things over to the new state manager, we need
// to separate out a common base class for the compression stream internal state and separate
// two separate impls that differ only in how they manage state. Once the autogate is removed,
// we can delete the first impl class and merge everything back together.
template <Context::Mode mode>
class CompressionStreamBase: public kj::Refcounted,
                             public kj::AsyncInputStream,
                             public capnp::ExplicitEndOutputStream {
 public:
  explicit CompressionStreamBase(kj::String format,
      Context::ContextFlags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
      : context(mode, format, flags, kj::mv(externalMemoryTarget)) {}

  // WritableStreamSink implementation ---------------------------------------------------

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override final {
    requireActive("Write after close");
    context.setInput(buffer.begin(), buffer.size());
    writeInternal(Z_NO_FLUSH);
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override final {
    // We check state here so that we catch errors even if pieces is empty.
    requireActive("Write after close");
    for (auto piece: pieces) {
      co_await write(piece);
    }
    co_return;
  }

  kj::Promise<void> end() override final {
    transitionToEnded();
    writeInternal(Z_FINISH);
    co_return;
  }

  kj::Promise<void> whenWriteDisconnected() override final {
    return kj::NEVER_DONE;
  }

  void abortWrite(kj::Exception&& reason) override final {
    cancelInternal(kj::mv(reason));
  }

  // AsyncInputStream implementation -----------------------------------------------------

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override final {
    KJ_ASSERT(minBytes <= maxBytes);
    // Re-throw any stored exception
    throwIfException();
    if (output.empty()) {
      // For brotli we defer trailing-data errors until buffered output is drained.
      if (context.hasTrailingError()) {
        auto ex =
            JSG_KJ_EXCEPTION(FAILED, TypeError, "Trailing bytes after end of compressed data");
        cancelInternal(kj::cp(ex));
        kj::throwFatalException(kj::mv(ex));
      }
      // If stream has ended normally and no buffered data, return EOF.
      if (isInTerminalState()) {
        co_return static_cast<size_t>(0);
      }
    }
    // Active or terminal with data remaining
    co_return co_await tryReadInternal(
        kj::arrayPtr(reinterpret_cast<kj::byte*>(buffer), maxBytes), minBytes);
  }

 protected:
  virtual void requireActive(kj::StringPtr errorMessage) = 0;
  virtual void transitionToEnded() = 0;
  virtual void transitionToErrored(kj::Exception&& reason) = 0;
  virtual void throwIfException() = 0;
  virtual bool isInTerminalState() = 0;

 private:
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
    transitionToErrored(kj::mv(reason));
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
    if (output.size() >= minBytes || isInTerminalState()) {
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
    KJ_ASSERT(flush == Z_FINISH || !isInTerminalState());
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

    if (isInTerminalState() && !pendingReads.empty()) {
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

  Context context;

  kj::Canceler canceler;
  LazyBuffer output;
  RingBuffer<PendingRead, 8> pendingReads;
};

// Uncompressed data goes in. Compressed data comes out.
// TODO(cleanup): Once the autogate is removed, delete this class and merge CompressionStreamBase
// and CompressionStreamImplV2 back into a single class.
template <Context::Mode mode>
class CompressionStreamImpl final: public CompressionStreamBase<mode> {
 public:
  explicit CompressionStreamImpl(kj::String format,
      Context::ContextFlags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
      : CompressionStreamBase<mode>(kj::mv(format), flags, kj::mv(externalMemoryTarget)) {}

 protected:
  void requireActive(kj::StringPtr errorMessage) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ended, Ended) {
        JSG_FAIL_REQUIRE(Error, errorMessage);
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        kj::throwFatalException(kj::cp(exception));
      }
      KJ_CASE_ONEOF(open, Open) {
        return;
      }
    }
    KJ_UNREACHABLE;
  }

  void transitionToEnded() override {
    state = Ended();
  }

  void transitionToErrored(kj::Exception&& reason) override {
    state = kj::mv(reason);
  }

  void throwIfException() override {
    KJ_IF_SOME(exception, state.template tryGet<kj::Exception>()) {
      kj::throwFatalException(kj::cp(exception));
    }
  }

  virtual bool isInTerminalState() override {
    // Ended or Exception are both terminal states.
    return state.template is<Ended>() || state.template is<kj::Exception>();
  }

 private:
  struct Ended {};
  struct Open {};

  kj::OneOf<Open, Ended, kj::Exception> state = Open();
};

template <Context::Mode mode>
class CompressionStreamImplV2 final: public CompressionStreamBase<mode> {
 public:
  explicit CompressionStreamImplV2(kj::String format,
      Context::ContextFlags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
      : CompressionStreamBase<mode>(kj::mv(format), flags, kj::mv(externalMemoryTarget)),
        state(decltype(state)::template create<Open>()) {}

 protected:
  void requireActive(kj::StringPtr errorMessage) override {
    KJ_IF_SOME(exception, state.tryGetErrorUnsafe()) {
      kj::throwFatalException(kj::cp(exception));
    }
    // isActive() returns true only if in Open state (the ActiveState)
    JSG_REQUIRE(state.isActive(), Error, errorMessage);
  }

  void transitionToEnded() override {
    // If already in a terminal state (Ended or Exception), this is a no-op.
    // This matches the V1 behavior where calling end() multiple times was allowed.
    if (state.isTerminal()) return;
    auto result = state.template transitionFromTo<Open, Ended>();
    KJ_REQUIRE(result != kj::none, "Stream already ended or errored");
  }

  void transitionToErrored(kj::Exception&& reason) override {
    // Use forceTransitionTo because cancelInternal may be called when already
    // in an error state (e.g., from writeInternal error handling).
    state.template forceTransitionTo<kj::Exception>(kj::mv(reason));
  }

  void throwIfException() override {
    KJ_IF_SOME(exception, state.tryGetErrorUnsafe()) {
      kj::throwFatalException(kj::cp(exception));
    }
  }

  virtual bool isInTerminalState() override {
    return state.isTerminal();
  }

 private:
  struct Ended {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "ended"_kj;
  };
  struct Open {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "open"_kj;
  };

  // State machine for tracking compression stream lifecycle:
  //   Open -> Ended (normal close via end())
  //   Open -> kj::Exception (error via abortWrite())
  // Ended is terminal, kj::Exception is implicitly terminal via ErrorState.
  StateMachine<TerminalStates<Ended>,
      ErrorState<kj::Exception>,
      ActiveState<Open>,
      Open,
      Ended,
      kj::Exception>
      state;
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
  explicit CompressionStreamAdapter(kj::Rc<CompressionStreamBase<mode>> impl)
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
  kj::Rc<CompressionStreamBase<mode>> impl;
  IoContext& ioContext;
};

kj::Rc<CompressionStreamBase<Context::Mode::COMPRESS>> createCompressionStreamImpl(
    kj::String format,
    Context::ContextFlags flags,
    kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget) {
  // TODO(cleanup): Once the autogate is removed, we can delete CompressionStreamImpl
  if (util::Autogate::isEnabled(util::AutogateKey::COMPRESSION_STREAM_USE_STATE_MACHINE)) {
    return kj::rc<CompressionStreamImplV2<Context::Mode::COMPRESS>>(
        kj::mv(format), flags, kj::mv(externalMemoryTarget));
  }
  return kj::rc<CompressionStreamImpl<Context::Mode::COMPRESS>>(
      kj::mv(format), flags, kj::mv(externalMemoryTarget));
}

kj::Rc<CompressionStreamBase<Context::Mode::DECOMPRESS>> createDecompressionStreamImpl(
    kj::String format,
    Context::ContextFlags flags,
    kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget) {
  // TODO(cleanup): Once the autogate is removed, we can delete CompressionStreamImpl
  if (util::Autogate::isEnabled(util::AutogateKey::COMPRESSION_STREAM_USE_STATE_MACHINE)) {
    return kj::rc<CompressionStreamImplV2<Context::Mode::DECOMPRESS>>(
        kj::mv(format), flags, kj::mv(externalMemoryTarget));
  }
  return kj::rc<CompressionStreamImpl<Context::Mode::DECOMPRESS>>(
      kj::mv(format), flags, kj::mv(externalMemoryTarget));
}

}  // namespace

jsg::Ref<CompressionStream> CompressionStream::constructor(jsg::Lock& js, kj::String format) {
  JSG_REQUIRE(
      format == "deflate" || format == "gzip" || format == "deflate-raw" || format == "brotli",
      TypeError,
      "The compression format must be either 'deflate', 'deflate-raw', 'gzip', or 'brotli'.");

  // TODO(cleanup): Once the autogate is removed, we can delete CompressionStreamImpl
  kj::Rc<CompressionStreamBase<Context::Mode::COMPRESS>> impl = createCompressionStreamImpl(
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
  JSG_REQUIRE(
      format == "deflate" || format == "gzip" || format == "deflate-raw" || format == "brotli",
      TypeError,
      "The compression format must be either 'deflate', 'deflate-raw', 'gzip', or 'brotli'.");

  kj::Rc<CompressionStreamBase<Context::Mode::DECOMPRESS>> impl =
      createDecompressionStreamImpl(kj::mv(format),
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
