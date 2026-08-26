// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compression.h"

#include "zlib-rs-bridge.h"

#include <workerd/io/features.h>
#include <workerd/jsg/util.h>
#include <workerd/util/autogate.h>

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
// ZlibBackend

namespace {

// Wrappers over the native (chromium) zlib. deflateInit2/inflateInit2 are
// macros injecting ZLIB_VERSION and sizeof(z_stream), hence the indirection.
int nativeDeflateInit2(z_stream* strm, int level, int windowBits, int memLevel, int strategy) {
  return deflateInit2(strm, level, Z_DEFLATED, windowBits, memLevel, strategy);
}
int nativeInflateInit2(z_stream* strm, int windowBits) {
  return inflateInit2(strm, windowBits);
}

constexpr ZlibBackend NATIVE_ZLIB = {
  .initDeflate = &nativeDeflateInit2,
  .initInflate = &nativeInflateInit2,
  .runDeflate = &deflate,
  .runInflate = &inflate,
  .endDeflate = &deflateEnd,
  .endInflate = &inflateEnd,
  .resetDeflate = &deflateReset,
  .resetInflate = &inflateReset,
  .setDeflateParams = &deflateParams,
  .setDeflateDictionary = &deflateSetDictionary,
  .setInflateDictionary = &inflateSetDictionary,
};

// Wrappers over zlib-rs; the bridge takes void* because its translation unit
// cannot see the chromium zlib types (see zlib-rs-bridge.h).
int rsDeflateInit2(z_stream* strm, int level, int windowBits, int memLevel, int strategy) {
  return zlibrs::initDeflate(strm, level, windowBits, memLevel, strategy);
}
int rsInflateInit2(z_stream* strm, int windowBits) {
  return zlibrs::initInflate(strm, windowBits);
}
int rsDeflate(z_stream* strm, int flush) {
  return zlibrs::runDeflate(strm, flush);
}
int rsInflate(z_stream* strm, int flush) {
  return zlibrs::runInflate(strm, flush);
}
int rsDeflateEnd(z_stream* strm) {
  return zlibrs::endDeflate(strm);
}
int rsInflateEnd(z_stream* strm) {
  return zlibrs::endInflate(strm);
}
int rsDeflateReset(z_stream* strm) {
  return zlibrs::resetDeflate(strm);
}
int rsInflateReset(z_stream* strm) {
  return zlibrs::resetInflate(strm);
}
int rsDeflateParams(z_stream* strm, int level, int strategy) {
  return zlibrs::setDeflateParams(strm, level, strategy);
}
int rsDeflateSetDictionary(z_stream* strm, const kj::byte* dictionary, uint32_t dictLength) {
  return zlibrs::setDeflateDictionary(strm, dictionary, dictLength);
}
int rsInflateSetDictionary(z_stream* strm, const kj::byte* dictionary, uint32_t dictLength) {
  return zlibrs::setInflateDictionary(strm, dictionary, dictLength);
}

constexpr ZlibBackend ZLIB_RS = {
  .initDeflate = &rsDeflateInit2,
  .initInflate = &rsInflateInit2,
  .runDeflate = &rsDeflate,
  .runInflate = &rsInflate,
  .endDeflate = &rsDeflateEnd,
  .endInflate = &rsInflateEnd,
  .resetDeflate = &rsDeflateReset,
  .resetInflate = &rsInflateReset,
  .setDeflateParams = &rsDeflateParams,
  .setDeflateDictionary = &rsDeflateSetDictionary,
  .setInflateDictionary = &rsInflateSetDictionary,
};

}  // namespace

const ZlibBackend& selectZlibBackend() {
  if (util::Autogate::isEnabled(util::AutogateKey::COMPRESSION_RS)) {
    return ZLIB_RS;
  }
  return NATIVE_ZLIB;
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
        return backend.initDeflate(
            &stream, options.level, options.windowBits, options.memLevel, options.strategy);
      case Mode::DECOMPRESS:
        return backend.initInflate(&stream, options.windowBits);
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
        return backend.resetDeflate(&stream);
      case Mode::DECOMPRESS:
        return backend.resetInflate(&stream);
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
      return backend.endDeflate(&stream);
    case Mode::DECOMPRESS:
      return backend.endInflate(&stream);
  }
  KJ_UNREACHABLE;
}

int ZlibStream::run(int flush) {
  KJ_ASSERT(initialized && !ended, "ZlibStream::run() requires a live stream");
  switch (mode) {
    case Mode::COMPRESS:
      return backend.runDeflate(&stream, flush);
    case Mode::DECOMPRESS:
      return backend.runInflate(&stream, flush);
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
      break;
  }

  return Result{
    .success = result == Z_OK,
    .result = result,
    .buffer = kj::arrayPtr(buffer, sizeof(buffer) - stream.availOut()),
  };
}

void CodecStage::Context::enforceStrictChecks(int flush, const Result& result) {
  if (stream.getMode() != Mode::DECOMPRESS || strictCompression != Flags::STRICT) {
    return;
  }
  // The spec requires that a TypeError is produced if there is trailing data after the end
  // of the compression stream. Called AFTER the caller has buffered the iteration's output:
  // the final valid bytes (produced by the very pump step that observed the trailing junk)
  // are still delivered to any read that consumes them before the error lands, which is the
  // WPT-pinned observable order.
  JSG_REQUIRE(!(result.result == Z_STREAM_END && stream.availIn() > 0), TypeError,
      "Trailing bytes after end of compressed data");
  // Same applies to closing a stream before the complete decompressed data is available.
  JSG_REQUIRE(!(flush == Z_FINISH && result.result == Z_BUF_ERROR && result.buffer.size() == 0),
      TypeError, "Called close() on a decompression stream with incomplete data");
}

kj::ArrayPtr<kj::byte> CodecStage::LazyBuffer::take(size_t readSize) {
  KJ_ASSERT(readSize <= validSize);
  // An empty read must not index the vector: the read offset is output.size() - validSize,
  // which is one past the end whenever the valid region is empty.
  if (readSize == 0) return nullptr;
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
  dest.write(output.take(n));
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
    // Buffer any produced output BEFORE the strict checks run: an iteration can both produce
    // the stream's final bytes and observe the strict-mode error condition (e.g. trailing
    // junk after the end of the compressed data), and the bytes must remain deliverable.
    if (result.buffer.size() > 0) {
      output.write(result.buffer);
    }
    context.enforceStrictChecks(flush, result);
    if (result.buffer.size() == 0) {
      if (result.success) {
        // No output produced but input data has been processed based on the zlib return
        // code; call pumpOnce again.
        continue;
      }
      return;
    }
  }
  KJ_UNREACHABLE;
}

// =======================================================================================
// CompressionCodec

CompressionCodec::CompressionCodec(CodecStage::Mode mode,
    kj::StringPtr format,
    CodecStage::Flags flags,
    kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
    : stage(mode, format, flags, kj::mv(externalMemoryTarget)) {}

void CompressionCodec::push(jsg::JsBufferSource chunk) {
  stage.push(chunk.asArrayPtr());
}

void CompressionCodec::end() {
  stage.end();
}

uint32_t CompressionCodec::pullInto(jsg::JsBufferSource view) {
  return static_cast<uint32_t>(stage.pull(view.asArrayPtr()));
}

double CompressionCodec::available() {
  return static_cast<double>(stage.available());
}

void newCompressionCodecCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  // liftKj converts thrown kj/jsg exceptions (e.g. the validation TypeErrors below) into JS
  // exceptions (without it they would escape the raw callback and take down the process) and
  // sets the returned value as the callback's return value.
  jsg::liftKj(info, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(info.GetIsolate());

    auto modeStr = JSG_REQUIRE_NONNULL(jsg::JsValue(info[0]).tryCast<jsg::JsString>(), TypeError,
        "newCompressionCodec() expects a string mode argument");
    auto formatStr = JSG_REQUIRE_NONNULL(jsg::JsValue(info[1]).tryCast<jsg::JsString>(), TypeError,
        "newCompressionCodec() expects a string format argument");
    auto mode = modeStr.toString(js);
    auto format = formatStr.toString(js);

    JSG_REQUIRE(format == "deflate" || format == "gzip" || format == "deflate-raw", TypeError,
        "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.");
    CodecStage::Mode codecMode;
    CodecStage::Flags codecFlags = CodecStage::Flags::NONE;
    if (mode == "compress") {
      codecMode = CodecStage::Mode::COMPRESS;
    } else if (mode == "decompress") {
      codecMode = CodecStage::Mode::DECOMPRESS;
      if (FeatureFlags::get(js).getStrictCompression()) {
        codecFlags = CodecStage::Flags::STRICT;
      }
    } else {
      JSG_FAIL_REQUIRE(TypeError, "The codec mode must be either 'compress' or 'decompress'.");
    }

    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<CompressionCodec>>());
    return handler.wrap(js,
        js.alloc<CompressionCodec>(codecMode, format, codecFlags, js.getExternalMemoryTarget()));
  });
}

// =======================================================================================
// Brotli / Zstd contexts

void BrotliContext::setBuffers(kj::ArrayPtr<kj::byte> input, kj::ArrayPtr<kj::byte> output) {
  nextIn = reinterpret_cast<const uint8_t*>(input.begin());
  nextOut = output.begin();
  availIn = input.size();
  availOut = output.size();
}

void BrotliContext::setInputBuffer(kj::ArrayPtr<const kj::byte> input) {
  nextIn = input.begin();
  availIn = input.size();
}

void BrotliContext::setOutputBuffer(kj::ArrayPtr<kj::byte> output) {
  nextOut = output.begin();
  availOut = output.size();
}

uint BrotliContext::getAvailOut() const {
  return availOut;
}

void BrotliContext::setFlush(int _flush) {
  flush = static_cast<BrotliEncoderOperation>(_flush);
}

void BrotliContext::getAfterWriteResult(uint32_t* _availIn, uint32_t* _availOut) const {
  *_availIn = availIn;
  *_availOut = availOut;
}

BrotliEncoderContext::BrotliEncoderContext(CompressionAllocator& allocator, ZlibMode _mode)
    : BrotliContext(allocator, _mode) {
  // NOTE: Ignores any returned errors.
  // TODO(soon): It's possible that initialization doesn't need to happen until `initialize` is
  //   called elsewhere. I'm keeping it like this to avoid changing the existing behaviour.
  auto _ = initialize();
}

void BrotliEncoderContext::work() {
  JSG_REQUIRE(mode == ZlibMode::BROTLI_ENCODE, Error, "Mode should be BROTLI_ENCODE"_kj);
  JSG_REQUIRE_NONNULL(state.get(), Error, "State should not be empty"_kj);

  const uint8_t* internalNext = nextIn;
  lastResult = BrotliEncoderCompressStream(
      state.get(), flush, &availIn, &internalNext, &availOut, &nextOut, nullptr);
  nextIn += internalNext - nextIn;

  streamEnd = lastResult && BrotliEncoderIsFinished(state.get());
}

kj::Maybe<CompressionError> BrotliEncoderContext::initialize() {
  auto instance = BrotliEncoderCreateInstance(
      CompressionAllocator::AllocForBrotli, CompressionAllocator::FreeForZlib, &allocator);
  state = kj::disposeWith<BrotliEncoderDestroyInstance>(kj::mv(instance));

  if (state.get() == nullptr) {
    return CompressionError(
        "Could not initialize Brotli instance"_kj, "ERR_ZLIB_INITIALIZATION_FAILED"_kj, -1);
  }

  return kj::none;
}

kj::Maybe<CompressionError> BrotliEncoderContext::resetStream() {
  return initialize();
}

kj::Maybe<CompressionError> BrotliEncoderContext::setParams(int key, uint32_t value) {
  if (!BrotliEncoderSetParameter(state.get(), static_cast<BrotliEncoderParameter>(key), value)) {
    return CompressionError("Setting parameter failed", "ERR_BROTLI_PARAM_SET_FAILED", -1);
  }

  return kj::none;
}

kj::Maybe<CompressionError> BrotliEncoderContext::getError() const {
  if (!lastResult) {
    return CompressionError("Compression failed", "ERR_BROTLI_COMPRESSION_FAILED", -1);
  }

  return kj::none;
}

bool BrotliEncoderContext::isStreamEnd() const {
  return streamEnd;
}

BrotliDecoderContext::BrotliDecoderContext(CompressionAllocator& allocator, ZlibMode _mode)
    : BrotliContext(allocator, _mode) {
  // NOTE: Ignores any returned errors.
  // TODO(soon): It's possible that initialization doesn't need to happen until `initialize` is
  //   called elsewhere. I'm keeping it like this to avoid changing the existing behaviour.
  auto _ = initialize();
}

kj::Maybe<CompressionError> BrotliDecoderContext::initialize() {
  auto instance = BrotliDecoderCreateInstance(
      CompressionAllocator::AllocForBrotli, CompressionAllocator::FreeForZlib, &allocator);
  state = kj::disposeWith<BrotliDecoderDestroyInstance>(kj::mv(instance));

  if (state.get() == nullptr) {
    return CompressionError(
        "Could not initialize Brotli instance", "ERR_ZLIB_INITIALIZATION_FAILED", -1);
  }

  return kj::none;
}

void BrotliDecoderContext::work() {
  JSG_REQUIRE(mode == ZlibMode::BROTLI_DECODE, Error, "Mode should have been BROTLI_DECODE"_kj);
  JSG_REQUIRE_NONNULL(state.get(), Error, "State should not be empty"_kj);
  const uint8_t* internalNext = nextIn;
  lastResult = BrotliDecoderDecompressStream(
      state.get(), &availIn, &internalNext, &availOut, &nextOut, nullptr);
  nextIn += internalNext - nextIn;

  if (lastResult == BROTLI_DECODER_RESULT_ERROR) {
    error = BrotliDecoderGetErrorCode(state.get());
    errorString = kj::str("ERR_", BrotliDecoderErrorString(error));
  }
}

kj::Maybe<CompressionError> BrotliDecoderContext::resetStream() {
  return initialize();
}

kj::Maybe<CompressionError> BrotliDecoderContext::setParams(int key, uint32_t value) {
  if (!BrotliDecoderSetParameter(state.get(), static_cast<BrotliDecoderParameter>(key), value)) {
    return CompressionError("Setting parameter failed", "ERR_BROTLI_PARAM_SET_FAILED", -1);
  }

  return kj::none;
}

kj::Maybe<CompressionError> BrotliDecoderContext::getError() const {
  if (error != BROTLI_DECODER_NO_ERROR) {
    return CompressionError("Compression failed", errorString, -1);
  }

  if (flush == BROTLI_OPERATION_FINISH && lastResult == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
    // Match zlib behavior, as brotli doesn't have its own code for this.
    return CompressionError("Unexpected end of file", "Z_BUF_ERROR", Z_BUF_ERROR);
  }

  return kj::none;
}

bool BrotliDecoderContext::isStreamEnd() const {
  return lastResult == BROTLI_DECODER_RESULT_SUCCESS;
}

// =======================================================================================
// Zstd Implementation

void ZstdContext::setBuffers(kj::ArrayPtr<kj::byte> input, kj::ArrayPtr<kj::byte> output) {
  setInputBuffer(input);
  setOutputBuffer(output);
}

void ZstdContext::setInputBuffer(kj::ArrayPtr<const kj::byte> input) {
  input_.src = input.begin();
  input_.size = input.size();
  input_.pos = 0;
}

void ZstdContext::setOutputBuffer(kj::ArrayPtr<kj::byte> output) {
  output_.dst = output.begin();
  output_.size = output.size();
  output_.pos = 0;
}

void ZstdContext::setFlush(int flush) {
  KJ_DASSERT(flush >= ZSTD_e_continue && flush <= ZSTD_e_end,
      "flush must be a valid ZSTD_EndDirective value");
  flush_ = static_cast<ZSTD_EndDirective>(flush);
}

kj::uint ZstdContext::getAvailOut() const {
  return output_.size - output_.pos;
}

void ZstdContext::getAfterWriteResult(uint32_t* availIn, uint32_t* availOut) const {
  *availIn = input_.size - input_.pos;
  *availOut = output_.size - output_.pos;
}

namespace {
// Helper to check ZSTD errors and return a CompressionError if present.
// Also sets the error code in the provided reference for later retrieval.
kj::Maybe<CompressionError> zstdCheckError(
    size_t result, ZSTD_ErrorCode& error, kj::StringPtr errorCode) {
  if (ZSTD_isError(result)) {
    error = ZSTD_getErrorCode(result);
    return CompressionError(ZSTD_getErrorName(result), errorCode, -1);
  }
  return kj::none;
}

// Wrappers for ZSTD free functions that return void (for use with kj::disposeWith).
void zstdFreeCCtx(ZSTD_CCtx* cctx) {
  ZSTD_freeCCtx(cctx);
}
void zstdFreeDCtx(ZSTD_DCtx* dctx) {
  ZSTD_freeDCtx(dctx);
}
}  // namespace

ZstdEncoderContext::ZstdEncoderContext(ZlibMode _mode)
    : ZstdContext(_mode),
      cctx_(kj::disposeWith<zstdFreeCCtx>(ZSTD_createCCtx())) {}

kj::Maybe<CompressionError> ZstdEncoderContext::initialize(uint64_t pledgedSrcSize) {
  if (cctx_.get() == nullptr) {
    return CompressionError(
        "Could not initialize Zstd instance"_kj, "ERR_ZLIB_INITIALIZATION_FAILED"_kj, -1);
  }

  if (pledgedSrcSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    size_t result = ZSTD_CCtx_setPledgedSrcSize(cctx_.get(), pledgedSrcSize);
    KJ_IF_SOME(err, zstdCheckError(result, error_, "ERR_ZSTD_COMPRESSION_FAILED"_kj)) {
      return kj::mv(err);
    }
  }

  return kj::none;
}

void ZstdEncoderContext::work() {
  JSG_REQUIRE(mode == ZlibMode::ZSTD_ENCODE, Error, "Mode should be ZSTD_ENCODE"_kj);
  JSG_REQUIRE(cctx_.get() != nullptr, Error, "Zstd context should not be null"_kj);

  lastResult = ZSTD_compressStream2(cctx_.get(), &output_, &input_, flush_);

  if (ZSTD_isError(lastResult)) {
    error_ = ZSTD_getErrorCode(lastResult);
  }
}

kj::Maybe<CompressionError> ZstdEncoderContext::resetStream() {
  if (cctx_.get() != nullptr) {
    size_t result = ZSTD_CCtx_reset(cctx_.get(), ZSTD_reset_session_only);
    KJ_IF_SOME(err, zstdCheckError(result, error_, "ERR_ZSTD_COMPRESSION_FAILED"_kj)) {
      return kj::mv(err);
    }
  }
  return kj::none;
}

kj::Maybe<CompressionError> ZstdEncoderContext::setParams(int key, int value) {
  KJ_DASSERT(key >= ZSTD_c_compressionLevel,
      "key must be a valid ZSTD_cParameter (first valid value is ZSTD_c_compressionLevel)");
  size_t result = ZSTD_CCtx_setParameter(cctx_.get(), static_cast<ZSTD_cParameter>(key), value);
  if (ZSTD_isError(result)) {
    return CompressionError(kj::str("Setting parameter failed: ", ZSTD_getErrorName(result)),
        "ERR_ZSTD_PARAM_SET_FAILED"_kj, -1);
  }
  return kj::none;
}

kj::Maybe<CompressionError> ZstdEncoderContext::getError() const {
  if (error_ != ZSTD_error_no_error) {
    return CompressionError(kj::str("Zstd compression failed: ", ZSTD_getErrorString(error_)),
        kj::str("ERR_ZSTD_COMPRESSION_FAILED"), -1);
  }

  if (flush_ == ZSTD_e_end && lastResult != 0) {
    // lastResult > 0 means more output is needed, which shouldn't happen at end
    return CompressionError("Unexpected end of file"_kj, "Z_BUF_ERROR"_kj, Z_BUF_ERROR);
  }

  return kj::none;
}

bool ZstdEncoderContext::isStreamEnd() const {
  // ZSTD_compressStream2 returns 0 when flush_ == ZSTD_e_end and the frame is fully flushed.
  return !ZSTD_isError(lastResult) && lastResult == 0;
}

ZstdDecoderContext::ZstdDecoderContext(ZlibMode _mode)
    : ZstdContext(_mode),
      dctx_(kj::disposeWith<zstdFreeDCtx>(ZSTD_createDCtx())) {}

kj::Maybe<CompressionError> ZstdDecoderContext::initialize() {
  // dctx_ is created in the constructor. It can only be nullptr if ZSTD_createDCtx()
  // failed due to memory allocation failure.
  if (dctx_.get() == nullptr) {
    return CompressionError(
        "Could not initialize Zstd instance"_kj, "ERR_ZLIB_INITIALIZATION_FAILED"_kj, -1);
  }

  return kj::none;
}

void ZstdDecoderContext::work() {
  JSG_REQUIRE(mode == ZlibMode::ZSTD_DECODE, Error, "Mode should be ZSTD_DECODE"_kj);
  JSG_REQUIRE(dctx_.get() != nullptr, Error, "Zstd context should not be null"_kj);

  lastResult = ZSTD_decompressStream(dctx_.get(), &output_, &input_);

  if (ZSTD_isError(lastResult)) {
    error_ = ZSTD_getErrorCode(lastResult);
  } else if (input_.size > 0) {
    // Track whether we're mid-frame: lastResult > 0 means more data needed,
    // lastResult == 0 means frame is complete.
    frameInProgress_ = (lastResult > 0);
  }
}

kj::Maybe<CompressionError> ZstdDecoderContext::resetStream() {
  if (dctx_.get() != nullptr) {
    size_t result = ZSTD_DCtx_reset(dctx_.get(), ZSTD_reset_session_only);
    KJ_IF_SOME(err, zstdCheckError(result, error_, "ERR_ZSTD_DECOMPRESSION_FAILED"_kj)) {
      return kj::mv(err);
    }
  }
  frameInProgress_ = false;
  return kj::none;
}

kj::Maybe<CompressionError> ZstdDecoderContext::setParams(int key, int value) {
  KJ_DASSERT(dctx_.get() != nullptr, "Zstd decompression context should not be null");
  size_t result = ZSTD_DCtx_setParameter(dctx_.get(), static_cast<ZSTD_dParameter>(key), value);
  if (ZSTD_isError(result)) {
    return CompressionError(kj::str("Setting parameter failed: ", ZSTD_getErrorName(result)),
        "ERR_ZSTD_PARAM_SET_FAILED"_kj, -1);
  }
  return kj::none;
}

kj::Maybe<CompressionError> ZstdDecoderContext::getError() const {
  if (error_ != ZSTD_error_no_error) {
    return CompressionError(kj::str("Zstd decompression failed: ", ZSTD_getErrorString(error_)),
        kj::str("ERR_ZSTD_DECOMPRESSION_FAILED"), -1);
  }

  // If this is the final flush, we're mid-frame (frame was started but never
  // completed), and the output buffer is not full (decoder had space but
  // couldn't produce more output), the input was truncated.
  if (flush_ == ZSTD_e_end && frameInProgress_ && output_.pos < output_.size) {
    return CompressionError("unexpected end of file"_kj, "ERR_ZSTD_DECOMPRESSION_FAILED"_kj, -1);
  }

  return kj::none;
}

bool ZstdDecoderContext::isStreamEnd() const {
  // ZSTD_decompressStream returns 0 when a frame is completely decoded and fully flushed.
  return !ZSTD_isError(lastResult) && lastResult == 0;
}

}  // namespace workerd::api
