// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compression.h"
#include <workerd/io/features.h>
#include <zlib.h>
#include <deque>
#include <vector>
#include <iterator>

namespace workerd::api {

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

  explicit Context(Mode mode, kj::StringPtr format, ContextFlags flags) :
      mode(mode), strictCompression(flags) {
    int result = Z_OK;
    switch (mode) {
      case Mode::COMPRESS:
        result = deflateInit2(
            &ctx,
            Z_DEFAULT_COMPRESSION,
            Z_DEFLATED,
            getWindowBits(format),
            8,  // memLevel = 8 is the default
            Z_DEFAULT_STRATEGY);
        break;
      case Mode::DECOMPRESS:
        result = inflateInit2(&ctx, getWindowBits(format));
        break;
      default:
        KJ_UNREACHABLE;
    }
    JSG_REQUIRE(result == Z_OK, Error, "Failed to initialize compression context.");
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
        JSG_REQUIRE(result == Z_OK || result == Z_BUF_ERROR || result == Z_STREAM_END,
                     Error,
                     "Compression failed.");
        break;
      case Mode::DECOMPRESS:
        result = inflate(&ctx, flush);
        JSG_REQUIRE(result == Z_OK || result == Z_BUF_ERROR || result == Z_STREAM_END,
                     Error,
                     "Decompression failed.");

        if (strictCompression == ContextFlags::STRICT) {
          // The spec requires that a TypeError is produced if there is trailing data after the end
          // of the compression stream.
          JSG_REQUIRE(!(result == Z_STREAM_END && ctx.avail_in > 0), TypeError,
              "Trailing bytes after end of compressed data");
          // Same applies to closing a stream before the complete decompressed data is available.
          JSG_REQUIRE(!(flush == Z_FINISH && result == Z_BUF_ERROR &&
              ctx.avail_out == sizeof(buffer)), TypeError,
              "Called close() on a decompression stream with incomplete data");
        }
        break;
      default:
        KJ_UNREACHABLE;
    }

    return Result {
      .success = result == Z_OK,
      .buffer = kj::arrayPtr(buffer, sizeof(buffer) - ctx.avail_out),
    };
  }

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
    if (format == "gzip") return DEFLATE + GZIP;
    else if (format == "deflate") return DEFLATE;
    else if (format == "deflate-raw") return DEFLATE_RAW;
    KJ_UNREACHABLE;
  }

  Mode mode;
  z_stream ctx = {};
  kj::byte buffer[4096];

  // For the eponymous compatibility flag
  ContextFlags strictCompression;
};

template <Context::Mode mode>
class CompressionStreamImpl: public kj::Refcounted,
                             public ReadableStreamSource,
                             public WritableStreamSink {
  // Uncompressed data goes in. Compressed data comes out.
public:
  explicit CompressionStreamImpl(kj::String format, Context::ContextFlags flags)
      : context(mode, format, flags) {}

  // WritableStreamSink implementation ---------------------------------------------------

  kj::Promise<void> write(const void* buffer, size_t size) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ended, Ended) {
        return JSG_KJ_EXCEPTION(FAILED, Error, "Write after close.");
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        return kj::cp(exception);
      }
      KJ_CASE_ONEOF(open, Open) {
        context.setInput(buffer, size);
        return writeInternal(Z_NO_FLUSH);
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ended, Ended) {
        return JSG_KJ_EXCEPTION(FAILED, Error, "Write after close.");
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        return kj::cp(exception);
      }
      KJ_CASE_ONEOF(open, Open) {
        if (pieces.size() == 0) return kj::READY_NOW;
        return write(pieces[0].begin(), pieces[0].size()).then([this, pieces]() {
          return write(pieces.slice(1, pieces.size()));
        });
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> end() override {
    state = Ended();
    return writeInternal(Z_FINISH);
  }

  void abort(kj::Exception reason) override {
    cancelInternal(kj::mv(reason));
  }

  // ReadableStreamSource implementation -------------------------------------------------

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    KJ_ASSERT(minBytes <= maxBytes);
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ended, Ended) {
        // There might still be data in the output buffer remaining to read.
        if (output.empty()) return size_t(0);
        return tryReadInternal(
            kj::ArrayPtr<kj::byte>(reinterpret_cast<kj::byte*>(buffer), maxBytes),
            minBytes);
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        return kj::cp(exception);
      }
      KJ_CASE_ONEOF(open, Open) {
        return tryReadInternal(
            kj::ArrayPtr<kj::byte>(reinterpret_cast<kj::byte*>(buffer), maxBytes),
            minBytes);
      }
    }
    KJ_UNREACHABLE;
  }

  void cancel(kj::Exception reason) override {
    cancelInternal(kj::mv(reason));
  }

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
    state = kj::mv(reason);
  }

  kj::Promise<size_t> tryReadInternal(kj::ArrayPtr<kj::byte> dest, size_t minBytes) {
    const auto copyIntoBuffer = [this](kj::ArrayPtr<kj::byte> dest) {
      auto maxBytesToCopy = kj::min(dest.size(), output.size());
      auto src = &output[0];
      memcpy(dest.begin(), src, maxBytesToCopy);
      output.erase(output.begin(), output.begin() + maxBytesToCopy);
      return maxBytesToCopy;
    };

    // If the output currently contains >= minBytes, then we'll fulfill
    // the read immediately, removing as many bytes as possible from the
    // output queue.
    if (output.size() >= minBytes) {
      return copyIntoBuffer(dest);
    }

    // Otherwise, create a pending read.
    auto promise = kj::newPromiseAndFulfiller<size_t>();
    auto pendingRead = PendingRead {
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

    return canceler.wrap(kj::mv(promise.promise));
  }

  kj::Promise<void> writeInternal(int flush) {
    // TODO(later): This does not yet implement any backpressure. A caller can keep calling
    // write without reading, which will continue to fill the internal buffer.
    KJ_ASSERT(flush == Z_FINISH || state.template is<Open>());
    Context::Result result;
    KJ_IF_MAYBE(exception, kj::runCatchingExceptions([this, flush, &result]() {
      result = context.pumpOnce(flush);
    })) {
      cancelInternal(kj::cp(*exception));
      return kj::mv(*exception);
    }

    if (result.buffer.size() == 0) {
      if (result.success) {
        return writeInternal(flush);
      }
      return maybeFulfillRead();
    }

    if (result.buffer.size() > 0) {
      std::copy(result.buffer.begin(), result.buffer.end(), std::back_inserter(output));
    }
    return writeInternal(flush);
  }

  kj::Promise<void> maybeFulfillRead() {
    // Fulfill as many pending reads as we can from the output buffer.
    auto remaining = output.size();
    auto source = output.begin();

    // If there are pending reads and data to be read, we'll loop through
    // the pending reads and fulfill them as much as possible.
    while (!pendingReads.empty() && remaining > 0) {
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
          return kj::mv(ex);
        }

        auto ex = JSG_KJ_EXCEPTION(FAILED, Error, "The pending read was canceled.");
        cancelInternal(kj::cp(ex));
        return kj::mv(ex);
      }

      // The pending read is still viable so determine how much we can copy in.
      auto amountToCopy = kj::min(pending.buffer.size() - pending.filled, remaining);
      std::copy(source, source + amountToCopy, pending.buffer.begin() + pending.filled);
      source += amountToCopy;
      pending.filled += amountToCopy;
      remaining -= amountToCopy;
      output.erase(output.begin(), source);

      // If we've met the minimum bytes requirement for the pending read, fulfill
      // the read promise.
      if (pending.filled >= pending.minBytes) {
        auto p = kj::mv(pending);
        pendingReads.pop_front();
        p.promise->fulfill(kj::mv(pending.filled));
        continue;
      }

      // If we reached this point in the loop, remaining must be 0 so that we
      // don't keep iterating through on the same pending read.
      KJ_ASSERT(remaining == 0);
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

    return kj::READY_NOW;
  }

  struct Ended {};
  struct Open {};

  kj::OneOf<Open, Ended, kj::Exception> state = Open();
  Context context;

  kj::Canceler canceler;
  std::vector<kj::byte> output;
  std::deque<PendingRead> pendingReads;
};
}  // namespace

jsg::Ref<CompressionStream> CompressionStream::constructor(jsg::Lock& js, kj::String format) {
  JSG_REQUIRE(format == "deflate" || format == "gzip" || format == "deflate-raw", TypeError,
               "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.");

  auto readableSide =
      kj::refcounted<CompressionStreamImpl<Context::Mode::COMPRESS>>(kj::mv(format),
                                                                     Context::ContextFlags::NONE);
  auto writableSide = kj::addRef(*readableSide);

  auto& ioContext = IoContext::current();

  return jsg::alloc<CompressionStream>(
    jsg::alloc<ReadableStream>(ioContext, kj::mv(readableSide)),
    jsg::alloc<WritableStream>(ioContext, kj::mv(writableSide)));
}

jsg::Ref<DecompressionStream> DecompressionStream::constructor(jsg::Lock& js, kj::String format) {
  JSG_REQUIRE(format == "deflate" || format == "gzip" || format == "deflate-raw", TypeError,
               "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.");

  auto readableSide =
      kj::refcounted<CompressionStreamImpl<Context::Mode::DECOMPRESS>>(
          kj::mv(format),
          FeatureFlags::get(js).getStrictCompression() ?
              Context::ContextFlags::STRICT :
              Context::ContextFlags::NONE);
  auto writableSide = kj::addRef(*readableSide);

  auto& ioContext = IoContext::current();

  return jsg::alloc<DecompressionStream>(
    jsg::alloc<ReadableStream>(ioContext, kj::mv(readableSide)),
    jsg::alloc<WritableStream>(ioContext, kj::mv(writableSide)));
}

}  // namespace workerd::api
