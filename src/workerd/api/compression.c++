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

}  // namespace workerd::api
