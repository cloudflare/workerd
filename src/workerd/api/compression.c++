// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compression.h"

#include <nbytes.h>

namespace workerd::api {

CompressionAllocator::CompressionAllocator(
    kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
    : externalMemoryTarget(kj::mv(externalMemoryTarget)) {}

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

// =======================================================================================
// ZlibStream

ZlibStream::ZlibStream(CompressionAllocator& allocator) {
  stream.zalloc = CompressionAllocator::AllocForZlib;
  stream.zfree = CompressionAllocator::FreeForZlib;
  stream.opaque = &allocator;
}

ZlibStream::~ZlibStream() noexcept(false) {
  end();
}

kj::Maybe<int> ZlibStream::init(Mode mode, Options options) {
  KJ_ASSERT(!initialized, "ZlibStream::init() may only be called once");
  this->mode = mode;
  int result = [&]() {
    switch (mode) {
      case Mode::COMPRESS:
        return deflateInit2(&stream, options.level, Z_DEFLATED, options.windowBits,
            options.memLevel, options.strategy);
      case Mode::DECOMPRESS:
        return inflateInit2(&stream, options.windowBits);
    }
    KJ_UNREACHABLE;
  }();
  if (result != Z_OK) {
    return result;
  }
  initialized = true;
  return kj::none;
}

kj::Maybe<int> ZlibStream::reset() {
  KJ_ASSERT(initialized, "ZlibStream::reset() requires an initialized stream");
  int result = [&]() {
    switch (mode) {
      case Mode::COMPRESS:
        return deflateReset(&stream);
      case Mode::DECOMPRESS:
        return inflateReset(&stream);
    }
    KJ_UNREACHABLE;
  }();
  if (result != Z_OK) {
    return result;
  }
  return kj::none;
}

int ZlibStream::end() {
  if (!initialized || ended) {
    return Z_OK;
  }
  ended = true;
  switch (mode) {
    case Mode::COMPRESS:
      return deflateEnd(&stream);
    case Mode::DECOMPRESS:
      return inflateEnd(&stream);
  }
  KJ_UNREACHABLE;
}

int ZlibStream::run(int flush) {
  KJ_ASSERT(initialized && !ended, "ZlibStream::run() requires a live stream");
  switch (mode) {
    case Mode::COMPRESS:
      return deflate(&stream, flush);
    case Mode::DECOMPRESS:
      return inflate(&stream, flush);
  }
  KJ_UNREACHABLE;
}

void ZlibStream::setInput(kj::ArrayPtr<const kj::byte> input) {
  // zlib's next_in is non-const for historical reasons; deflate/inflate do not write
  // through it.
  stream.next_in = const_cast<kj::byte*>(input.begin());
  stream.avail_in = input.size();
}

void ZlibStream::setOutput(kj::ArrayPtr<kj::byte> output) {
  stream.next_out = output.begin();
  stream.avail_out = output.size();
}

size_t ZlibStream::availIn() const {
  return stream.avail_in;
}

size_t ZlibStream::availOut() const {
  return stream.avail_out;
}

kj::StringPtr ZlibStream::msg() const {
  if (stream.msg == nullptr) return nullptr;
  return kj::StringPtr(stream.msg);
}

kj::StringPtr ZlibStream::errorCodeName(int code) {
  switch (code) {
    case Z_OK:
      return "Z_OK"_kj;
    case Z_STREAM_END:
      return "Z_STREAM_END"_kj;
    case Z_NEED_DICT:
      return "Z_NEED_DICT"_kj;
    case Z_ERRNO:
      return "Z_ERRNO"_kj;
    case Z_STREAM_ERROR:
      return "Z_STREAM_ERROR"_kj;
    case Z_DATA_ERROR:
      return "Z_DATA_ERROR"_kj;
    case Z_MEM_ERROR:
      return "Z_MEM_ERROR"_kj;
    case Z_BUF_ERROR:
      return "Z_BUF_ERROR"_kj;
    case Z_VERSION_ERROR:
      return "Z_VERSION_ERROR"_kj;
    default:
      return "Z_UNKNOWN_ERROR"_kj;
  }
}

kj::Maybe<int> ZlibStream::windowBitsForWebFormat(kj::StringPtr format) {
  // 15 is the default value of the windowBits parameter for zlib; adding 16 selects the
  // gzip wrapper, and negating selects a raw (headerless) stream.
  if (format == "gzip"_kj) return 15 + 16;
  if (format == "deflate"_kj) return 15;
  if (format == "deflate-raw"_kj) return -15;
  return kj::none;
}

// =======================================================================================
// CodecStage

CodecStage::Context::Context(Mode mode,
    kj::StringPtr format,
    Flags flags,
    kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
    : allocator(kj::mv(externalMemoryTarget)),
      stream(allocator),
      strictCompression(flags) {
  auto windowBits = KJ_ASSERT_NONNULL(ZlibStream::windowBitsForWebFormat(format));
  JSG_REQUIRE(stream.init(mode, ZlibStream::Options{.windowBits = windowBits}) == kj::none, Error,
      "Failed to initialize compression context."_kj);
}

void CodecStage::Context::setInput(const void* in, size_t size) {
  stream.setInput(kj::arrayPtr(reinterpret_cast<const kj::byte*>(in), size));
}

CodecStage::Context::Result CodecStage::Context::pumpOnce(int flush) {
  stream.setOutput(kj::arrayPtr(buffer, sizeof(buffer)));

  int result = stream.run(flush);

  switch (stream.getMode()) {
    case Mode::COMPRESS:
      JSG_REQUIRE(result == Z_OK || result == Z_BUF_ERROR || result == Z_STREAM_END, TypeError,
          "Compression failed.");
      break;
    case Mode::DECOMPRESS:
      JSG_REQUIRE(result == Z_OK || result == Z_BUF_ERROR || result == Z_STREAM_END, TypeError,
          "Decompression failed.");

      if (strictCompression == Flags::STRICT) {
        // The spec requires that a TypeError is produced if there is trailing data after the
        // end of the compression stream.
        JSG_REQUIRE(!(result == Z_STREAM_END && stream.availIn() > 0), TypeError,
            "Trailing bytes after end of compressed data");
        // Same applies to closing a stream before the complete decompressed data is
        // available.
        JSG_REQUIRE(
            !(flush == Z_FINISH && result == Z_BUF_ERROR && stream.availOut() == sizeof(buffer)),
            TypeError, "Called close() on a decompression stream with incomplete data");
      }
      break;
  }

  return Result{
    .success = result == Z_OK,
    .buffer = kj::arrayPtr(buffer, sizeof(buffer) - stream.availOut()),
  };
}

kj::ArrayPtr<kj::byte> CodecStage::LazyBuffer::take(size_t readSize) {
  KJ_ASSERT(readSize <= validSize);
  kj::ArrayPtr<kj::byte> chunk = kj::arrayPtr(&output[output.size() - validSize], readSize);
  validSize -= readSize;
  return chunk;
}

void CodecStage::LazyBuffer::maybeShift() {
  size_t unusedSpace = output.size() - validSize;
  if (unusedSpace >= 1024 && unusedSpace >= (output.size() >> 3)) {
    // Shifting buffer to erase data that has already been read. validSize remains the same.
    memmove(output.begin(), output.begin() + unusedSpace, validSize);
    output.truncate(validSize);
  }
}

void CodecStage::LazyBuffer::write(kj::ArrayPtr<const kj::byte> chunk) {
  output.addAll(chunk);
  validSize += chunk.size();
}

void CodecStage::LazyBuffer::clear() {
  output.clear();
  validSize = 0;
}

size_t CodecStage::LazyBuffer::size() {
  return validSize;
}

bool CodecStage::LazyBuffer::empty() {
  return validSize == 0;
}

CodecStage::CodecStage(Mode mode,
    kj::StringPtr format,
    Flags flags,
    kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
    : context(mode, format, flags, kj::mv(externalMemoryTarget)) {}

void CodecStage::push(kj::ArrayPtr<const kj::byte> input) {
  context.setInput(input.begin(), input.size());
  pump(Z_NO_FLUSH);
}

void CodecStage::end() {
  if (finished) return;
  finished = true;
  pump(Z_FINISH);
}

size_t CodecStage::pull(kj::ArrayPtr<kj::byte> dest) {
  auto n = kj::min(dest.size(), output.size());
  if (n == 0) return 0;
  dest.first(n).copyFrom(output.take(n));
  output.maybeShift();
  return n;
}

size_t CodecStage::available() {
  return output.size();
}

bool CodecStage::empty() {
  return output.empty();
}

void CodecStage::clear() {
  output.clear();
}

void CodecStage::pump(int flush) {
  while (true) {
    auto result = context.pumpOnce(flush);
    if (result.buffer.size() == 0) {
      if (result.success) {
        // No output produced but input data has been processed based on the zlib return
        // code; call pumpOnce again.
        continue;
      }
      return;
    }
    // Output has been produced: buffer it and pump again.
    output.write(result.buffer);
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::api
