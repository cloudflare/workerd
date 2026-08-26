// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "zlib-util.h"

#include "util.h"

// The following implementation is adapted from Node.js
// and therefore follows Node.js style as opposed to kj style.
// Latest implementation of Node.js zlib can be found at:
// https://github.com/nodejs/node/blob/main/src/node_zlib.cc
namespace workerd::api::node {

kj::ArrayPtr<const kj::byte> getInputFromSource(const ZlibUtil::InputSource& data) {
  KJ_SWITCH_ONEOF(data) {
    KJ_CASE_ONEOF(dataBuf, kj::Array<kj::byte>) {
      JSG_REQUIRE(dataBuf.size() < Z_MAX_CHUNK, RangeError, "Memory limit exceeded"_kj);
      return dataBuf.asPtr();
    }

    KJ_CASE_ONEOF(dataStr, jsg::NonCoercible<kj::String>) {
      JSG_REQUIRE(dataStr.value.size() < Z_MAX_CHUNK, RangeError, "Memory limit exceeded"_kj);
      return dataStr.value.asBytes();
    }
  }

  KJ_UNREACHABLE;
}

uint32_t ZlibUtil::crc32Sync(InputSource data, uint32_t value) {
  auto dataPtr = getInputFromSource(data);
  return crc32(value, dataPtr.begin(), dataPtr.size());
}

namespace {
class GrowableBuffer final {
  // A copy of kj::Vector with some additional methods for use as a growable buffer with a maximum
  // size
 public:
  inline explicit GrowableBuffer(size_t _chunkSize, size_t _maxCapacity)
      : maxCapacity(_maxCapacity) {
    auto maxChunkSize = kj::min(_chunkSize, maxCapacity);
    builder = kj::heapArrayBuilder<kj::byte>(maxChunkSize);
    chunkSize = maxChunkSize;
  }

  size_t size() const {
    return builder.size();
  }
  bool empty() const {
    return size() == 0;
  }
  size_t capacity() const {
    return builder.capacity();
  }
  size_t available() const {
    return capacity() - size();
  }

  kj::byte* begin() KJ_LIFETIMEBOUND {
    return builder.begin();
  }
  kj::byte* end() KJ_LIFETIMEBOUND {
    return builder.end();
  }

  kj::Array<kj::byte> releaseAsArray() {
    // TODO(perf):  Avoid a copy/move by allowing Array<T> to point to incomplete space?
    if (!builder.isFull()) {
      setCapacity(size());
    }
    return builder.finish();
  }

  void adjustUnused(size_t unused) {
    resize(capacity() - unused);
  }

  void resize(size_t size) {
    if (size > builder.capacity()) grow(size);
    builder.resize(size);
  }

  void addChunk() {
    reserve(size() + chunkSize);
  }

  void reserve(size_t size) {
    if (size > builder.capacity()) {
      grow(size);
    }
  }

  bool atMaxCapacity() const {
    return size() >= maxCapacity;
  }

 private:
  kj::ArrayBuilder<kj::byte> builder;
  size_t chunkSize;
  size_t maxCapacity;

  void grow(size_t minCapacity = 0) {
    JSG_REQUIRE(minCapacity <= maxCapacity, RangeError, "Memory limit exceeded");
    setCapacity(kj::min(maxCapacity, kj::max(minCapacity, capacity() == 0 ? 4 : capacity() * 2)));
  }
  void setCapacity(size_t newSize) {
    if (builder.size() > newSize) {
      builder.truncate(newSize);
    }

    kj::ArrayBuilder<kj::byte> newBuilder = kj::heapArrayBuilder<kj::byte>(newSize);
    newBuilder.addAll(kj::mv(builder));
    builder = kj::mv(newBuilder);
  }
};
}  // namespace

void ZlibContext::initialize(int _level,
    int _windowBits,
    int _memLevel,
    int _strategy,
    jsg::Optional<kj::Array<kj::byte>> _dictionary) {
  if (!((_windowBits == 0) &&
          (mode == ZlibMode::INFLATE || mode == ZlibMode::GUNZIP || mode == ZlibMode::UNZIP))) {
    JSG_ASSERT(_windowBits >= Z_MIN_WINDOWBITS && _windowBits <= Z_MAX_WINDOWBITS, RangeError,
        kj::str("The value of \"options.windowBits\" is out of range. It must be >= ",
            Z_MIN_WINDOWBITS, " and <= ", Z_MAX_WINDOWBITS, ". Received ", _windowBits));
  }

  JSG_REQUIRE(_level >= Z_MIN_LEVEL && _level <= Z_MAX_LEVEL, RangeError,
      kj::str("The value of \"options.level\" is out of range. It must be >= ", Z_MIN_LEVEL,
          " and <= ", Z_MAX_LEVEL, ". Received ", _level));
  JSG_REQUIRE(_memLevel >= Z_MIN_MEMLEVEL && _memLevel <= Z_MAX_MEMLEVEL, RangeError,
      kj::str("The value of \"options.memLevel\" is out of range. It must be >= ", Z_MIN_MEMLEVEL,
          " and <= ", Z_MAX_MEMLEVEL, ". Received ", _memLevel));
  JSG_REQUIRE(_strategy == Z_FILTERED || _strategy == Z_HUFFMAN_ONLY || _strategy == Z_RLE ||
          _strategy == Z_FIXED || _strategy == Z_DEFAULT_STRATEGY,
      Error, "invalid strategy"_kj);

  level = _level;
  windowBits = _windowBits;
  memLevel = _memLevel;
  strategy = _strategy;
  flush = Z_NO_FLUSH;
  err = Z_OK;

  switch (mode) {
    case ZlibMode::GZIP:
    case ZlibMode::GUNZIP:
      windowBits += 16;
      break;
    case ZlibMode::UNZIP:
      windowBits += 32;
      break;
    case ZlibMode::DEFLATERAW:
    case ZlibMode::INFLATERAW:
      windowBits *= -1;
      break;
    default:
      break;
  }

  KJ_IF_SOME(dict, _dictionary) {
    // Deep-copy the dictionary bytes into runtime-owned storage. The incoming
    // kj::Array<kj::byte> from jsg::asBytes() is a non-owning view into the
    // V8 BackingStore; if the JS-side ArrayBuffer is resizable, the caller can
    // shrink it to 0 before the deferred setDictionary() runs, leaving the
    // stored pointer dangling into PROT_NONE pages (SIGSEGV).
    dictionary = dict.clone();
  }
}

kj::Maybe<CompressionError> ZlibContext::getError() const {
  // Acceptable error states depend on the type of zlib stream.
  switch (err) {
    case Z_OK:
    case Z_BUF_ERROR:
      if (core.availOut() != 0 && flush == Z_FINISH) {
        return constructError("unexpected end of file"_kj);
      }
      break;
    case Z_STREAM_END:
      // normal statuses, not fatal
      break;
    case Z_NEED_DICT:
      if (dictionary.empty()) {
        return constructError("Missing dictionary"_kj);
      } else {
        return constructError("Bad dictionary"_kj);
      }
    default:
      // something else.
      return constructError("Zlib error");
  }

  return {};
}

kj::Maybe<CompressionError> ZlibContext::setDictionary() {
  if (dictionary.empty()) {
    return kj::none;
  }

  err = Z_OK;

  switch (mode) {
    case ZlibMode::DEFLATE:
    case ZlibMode::DEFLATERAW:
      err = core.getZlibBackend().setDeflateDictionary(
          &core.raw(), dictionary.begin(), dictionary.size());
      break;
    case ZlibMode::INFLATERAW:
      err = core.getZlibBackend().setInflateDictionary(
          &core.raw(), dictionary.begin(), dictionary.size());
      break;
    default:
      break;
  }

  if (err != Z_OK) {
    return constructError("Failed to set dictionary"_kj);
  }

  return kj::none;
}

bool ZlibContext::initializeZlib() {
  if (core.isInitialized()) {
    return false;
  }

  // The shared core wired the allocator hooks at construction; here we only pick the
  // compression direction and hand over the (mode-adjusted) parameters.
  auto direction = [&]() {
    switch (mode) {
      case ZlibMode::DEFLATE:
      case ZlibMode::GZIP:
      case ZlibMode::DEFLATERAW:
        return ZlibStream::Mode::COMPRESS;
      case ZlibMode::INFLATE:
      case ZlibMode::GUNZIP:
      case ZlibMode::INFLATERAW:
      case ZlibMode::UNZIP:
        return ZlibStream::Mode::DECOMPRESS;
      default:
        KJ_UNREACHABLE;
    }
  }();

  err = Z_OK;
  KJ_IF_SOME(code,
      core.init(direction,
          ZlibStream::Options{
            .windowBits = windowBits,
            .level = level,
            .memLevel = memLevel,
            .strategy = strategy,
          })) {
    err = code;
    dictionary.clear();
    mode = ZlibMode::NONE;
    return true;
  }

  setDictionary();
  return true;
}

kj::Maybe<CompressionError> ZlibContext::resetStream() {
  bool initialized_now = initializeZlib();
  if (initialized_now && err != Z_OK) {
    return constructError("Failed to init stream before reset");
  }
  err = Z_OK;
  switch (mode) {
    case ZlibMode::DEFLATE:
    case ZlibMode::DEFLATERAW:
    case ZlibMode::GZIP:
    case ZlibMode::INFLATE:
    case ZlibMode::INFLATERAW:
    case ZlibMode::GUNZIP:
      KJ_IF_SOME(code, core.reset()) {
        err = code;
      }
      break;
    default:
      break;
  }

  if (err != Z_OK) {
    return constructError("Failed to reset stream"_kj);
  }

  return setDictionary();
}

void ZlibContext::work() {
  bool initialized_now = initializeZlib();
  if (initialized_now && err != Z_OK) {
    return;
  }

  const Bytef* next_expected_header_byte = nullptr;
  auto& stream = core.raw();

  // If the avail_out is left at 0, then it means that it ran out
  // of room.  If there was avail_out left over, then it means
  // that all the input was consumed.
  switch (mode) {
    case ZlibMode::DEFLATE:
    case ZlibMode::GZIP:
    case ZlibMode::DEFLATERAW:
      err = core.run(flush);
      break;
    case ZlibMode::UNZIP:
      if (stream.avail_in > 0) {
        next_expected_header_byte = stream.next_in;
      }

      switch (gzip_id_bytes_read) {
        case 0:
          if (next_expected_header_byte == nullptr) {
            break;
          }

          if (*next_expected_header_byte == GZIP_HEADER_ID1) {
            gzip_id_bytes_read = 1;
            next_expected_header_byte++;

            if (stream.avail_in == 1) {
              // The only available byte was already read.
              break;
            }
          } else {
            mode = ZlibMode::INFLATE;
            break;
          }

          [[fallthrough]];
        case 1:
          if (next_expected_header_byte == nullptr) {
            break;
          }

          if (*next_expected_header_byte == GZIP_HEADER_ID2) {
            gzip_id_bytes_read = 2;
            mode = ZlibMode::GUNZIP;
          } else {
            // There is no actual difference between INFLATE and INFLATERAW
            // (after initialization).
            mode = ZlibMode::INFLATE;
          }

          break;
        default:
          JSG_FAIL_REQUIRE(Error, "Invalid number of gzip magic number bytes read");
      }

      [[fallthrough]];
    case ZlibMode::INFLATE:
    case ZlibMode::GUNZIP:
    case ZlibMode::INFLATERAW:
      err = core.run(flush);

      // If data was encoded with dictionary (INFLATERAW will have it set in
      // SetDictionary, don't repeat that here)
      if (mode != ZlibMode::INFLATERAW && err == Z_NEED_DICT && !dictionary.empty()) {
        // Load it
        err = core.getZlibBackend().setInflateDictionary(
            &core.raw(), dictionary.begin(), dictionary.size());
        if (err == Z_OK) {
          // And try to decode again
          err = core.run(flush);
        } else if (err == Z_DATA_ERROR) {
          // Both inflateSetDictionary() and inflate() return Z_DATA_ERROR.
          // Make it possible for After() to tell a bad dictionary from bad
          // input.
          err = Z_NEED_DICT;
        }
      }

      while (stream.avail_in > 0 && mode == ZlibMode::GUNZIP && err == Z_STREAM_END &&
          stream.next_in[0] != 0x00) {
        // Bytes remain in input buffer. Perhaps this is another compressed
        // member in the same archive, or just trailing garbage.
        // Trailing zero bytes are okay, though, since they are frequently
        // used for padding.

        resetStream();
        err = core.run(flush);
      }
      break;
    default:
      KJ_UNREACHABLE;
  }
}

kj::Maybe<CompressionError> ZlibContext::setParams(int _level, int _strategy) {
  bool initialized_now = initializeZlib();
  if (initialized_now && err != Z_OK) {
    return constructError("Failed to init stream before set parameters");
  }
  err = Z_OK;

  switch (mode) {
    case ZlibMode::DEFLATE:
    case ZlibMode::DEFLATERAW:
      err = core.getZlibBackend().setDeflateParams(&core.raw(), _level, _strategy);
      break;
    default:
      break;
  }

  if (err != Z_OK && err != Z_BUF_ERROR) {
    return constructError("Failed to set parameters");
  }

  return kj::none;
}

ZlibContext::~ZlibContext() noexcept(false) {
  if (!core.isInitialized()) {
    return;
  }

  // Modes that never initialized a stream had nothing to end; for the rest, the shared
  // core's end() dispatches deflateEnd/inflateEnd by direction, matching the historical
  // per-mode switch.
  auto status = core.end();

  JSG_REQUIRE(
      status == Z_OK || status == Z_DATA_ERROR, Error, "Uncaught error on closing zlib stream");
}

void ZlibContext::setBuffers(kj::ArrayPtr<kj::byte> input, kj::ArrayPtr<kj::byte> output) {
  core.setInput(input);
  core.setOutput(output);
}

void ZlibContext::setInputBuffer(kj::ArrayPtr<const kj::byte> input) {
  core.setInput(input);
}

void ZlibContext::setOutputBuffer(kj::ArrayPtr<kj::byte> output) {
  core.setOutput(output);
}

template <typename CompressionContext>
jsg::Ref<ZlibUtil::CompressionStream<CompressionContext>> ZlibUtil::CompressionStream<
    CompressionContext>::constructor(jsg::Lock& js, ZlibModeValue mode) {
  return js.alloc<CompressionStream>(static_cast<ZlibMode>(mode), js.getExternalMemoryTarget());
}

template <typename CompressionContext>
ZlibUtil::CompressionStream<CompressionContext>::~CompressionStream() {
  // This destructor runs from cppgc's noexcept finalizer (~CppgcShim); it
  // MUST NOT throw. A throwing assertion here crosses the noexcept boundary
  // and triggers std::terminate(), killing the entire workerd process.
  if (writing) {
    KJ_LOG(ERROR, "CompressionStream destroyed while writing=true; state machine bug");
    return;  // Skip close() — the stream state is inconsistent.
  }
  close();
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::emitError(
    jsg::Lock& js, const CompressionError& error) {
  KJ_IF_SOME(onError, errorHandler) {
    onError(js, error.err, kj::mv(error.code), kj::mv(error.message));
  }

  if (pending_close) {
    close();
  }
}

template <typename CompressionContext>
template <bool async>
void ZlibUtil::CompressionStream<CompressionContext>::writeStream(
    jsg::Lock& js, int flush, kj::ArrayPtr<kj::byte> input, kj::ArrayPtr<kj::byte> output) {
  JSG_REQUIRE(initialized, Error, "Writing before initializing"_kj);
  JSG_REQUIRE(!closed, Error, "Already finalized"_kj);
  JSG_REQUIRE(!writing, Error, "Writing is in progress"_kj);
  JSG_REQUIRE(!pending_close, Error, "Pending close"_kj);

  uint32_t availIn = 0;
  uint32_t availOut = 0;
  kj::Maybe<CompressionError> maybeError;
  {
    // Ensure `writing` is reset on any exception path so that the destructor's
    // check never fires due to a stuck flag. Without this, a throwing backend
    // (e.g. KJ_UNREACHABLE in initializeZlib()) leaves `writing` permanently
    // true, and the destructor's assertion crosses the noexcept ~CppgcShim()
    // boundary during V8 GC, triggering std::terminate().
    //
    // TODO(someday): If the asynchronous variant of this method actually worked asynchronously,
    //   this would need to remain true until after `updateWriteResult` so that calls to `close`
    //   can be properly deferred.
    KJ_DEFER({ writing = false; });

    // Clear buffer pointers from the compression context when this scope exits.
    // The input/output kj::Array parameters are backed by V8 BackingStores
    // whose lifetimes are tied to their JavaScript ArrayBuffer objects.
    KJ_DEFER({ context()->clearBuffers(); });

    writing = true;
    context()->setBuffers(input, output);
    context()->setFlush(flush);
    context()->work();
    context()->getAfterWriteResult(&availIn, &availOut);
    maybeError = context()->getError();
  }

  // These pointers may become invalid if any JS calls below invalidate the JS objects' underlying
  // buffers. Setting them to nullptr to make this obvious.
  input = nullptr;
  output = nullptr;

  KJ_IF_SOME(error, maybeError) {
    emitError(js, kj::mv(error));
    return;
  }

  updateWriteResult(js, availIn, availOut);

  if constexpr (async) {
    // Only the async variant of this method runs the callback.
    KJ_IF_SOME(cb, writeCallback) {
      cb(js);
    }

    // Only the async variant of this method can result in deferred `close` calls.
    // TODO(someday): This possibly can't actually be hit because `writing` is never true during any
    //   JS callbacks as a result of this method not truly being asynchronous?
    if (pending_close) {
      close();
    }
  }
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::close() {
  pending_close = writing;
  if (writing) {
    return;
  }
  closed = true;
  // Guard against closing an uninitialized stream. This can happen when the
  // destructor calls close() on a handle that was constructed but never had
  // initialize() called (e.g. via _handle.constructor). Using a non-throwing
  // early return instead of JSG_ASSERT avoids a fatal throw from the noexcept
  // cppgc destructor chain.
  if (!initialized) {
    return;
  }
  // Drop JS-heap refs eagerly so callers that explicitly close don't have to
  // wait for the cycle collector. visitForGc handles the unclosed case.
  writeCallback = kj::none;
  writeResult = kj::none;
  errorHandler = kj::none;
  // Context is closed on the destructor of the CompressionContext.
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::initializeStream(
    jsg::Lock& js, jsg::JsArrayBufferView& _writeResult, jsg::Function<void()> _writeCallback) {
  writeResult = _writeResult.addRef(js);
  writeCallback = kj::mv(_writeCallback);
  initialized = true;
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::updateWriteResult(
    jsg::Lock& js, uint32_t availIn, uint32_t availOut) {
  KJ_IF_SOME(wr, writeResult) {
    auto result = wr.getHandle(js);
    auto ptr = result.template asArrayPtr<uint32_t>();
    JSG_REQUIRE(ptr.size() >= 2, Error, "Invalid write result buffer"_kj);
    ptr[0] = availOut;
    ptr[1] = availIn;
  }
}

template <typename CompressionContext>
template <bool async>
void ZlibUtil::CompressionStream<CompressionContext>::write(jsg::Lock& js,
    int flush,
    jsg::Optional<jsg::JsBufferSource> input,
    uint32_t inputOffset,
    uint32_t inputLength,
    jsg::JsBufferSource output,
    uint32_t outputOffset,
    uint32_t outputLength) {
  if (flush != Z_NO_FLUSH && flush != Z_PARTIAL_FLUSH && flush != Z_SYNC_FLUSH &&
      flush != Z_FULL_FLUSH && flush != Z_FINISH && flush != Z_BLOCK) {
    JSG_FAIL_REQUIRE(Error, "Invalid flush value");
  }

  // Use default values if input is not determined
  if (input == kj::none) {
    inputLength = 0;
    inputOffset = 0;
  }

  auto outputBytes = output.asArrayPtr();
  kj::ArrayPtr<kj::byte> inputBytes;
  KJ_IF_SOME(i, input) {
    inputBytes = i.asArrayPtr();
  }

  // Check for integer overflow...
  JSG_REQUIRE(inputOffset + inputLength >= inputOffset, Error, "Input access is not within bounds");
  JSG_REQUIRE(
      outputOffset + outputLength >= outputOffset, Error, "Output access is not within bounds");
  JSG_REQUIRE(IsWithinBounds(inputOffset, inputLength, inputBytes.size()), Error,
      "Input access is not within bounds"_kj);
  JSG_REQUIRE(IsWithinBounds(outputOffset, outputLength, outputBytes.size()), Error,
      "Output access is not within bounds"_kj);

  writeStream<async>(js, flush, inputBytes.slice(inputOffset, inputOffset + inputLength),
      outputBytes.slice(outputOffset, outputOffset + outputLength));
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::reset(jsg::Lock& js) {
  KJ_IF_SOME(error, context()->resetStream()) {
    emitError(js, kj::mv(error));
  }
}

jsg::Ref<ZlibUtil::ZlibStream> ZlibUtil::ZlibStream::constructor(
    jsg::Lock& js, ZlibModeValue mode) {
  auto m = static_cast<ZlibMode>(mode);
  JSG_REQUIRE(m == ZlibMode::DEFLATE || m == ZlibMode::INFLATE || m == ZlibMode::GZIP ||
          m == ZlibMode::GUNZIP || m == ZlibMode::DEFLATERAW || m == ZlibMode::INFLATERAW ||
          m == ZlibMode::UNZIP,
      TypeError, "Invalid zlib mode"_kj);
  return js.alloc<ZlibStream>(m, js.getExternalMemoryTarget());
}

void ZlibUtil::ZlibStream::initialize(jsg::Lock& js,
    int windowBits,
    int level,
    int memLevel,
    int strategy,
    jsg::JsArrayBufferView writeState,
    jsg::Function<void()> writeCallback,
    jsg::Optional<kj::Array<kj::byte>> dictionary) {
  initializeStream(js, writeState, kj::mv(writeCallback));
  context()->initialize(level, windowBits, memLevel, strategy, kj::mv(dictionary));
}

void ZlibUtil::ZlibStream::params(jsg::Lock& js, int _level, int _strategy) {
  context()->setParams(_level, _strategy);
  KJ_IF_SOME(err, context()->getError()) {
    emitError(js, kj::mv(err));
  }
}

template <typename CompressionContext>
jsg::Ref<ZlibUtil::ZstdCompressionStream<CompressionContext>> ZlibUtil::ZstdCompressionStream<
    CompressionContext>::constructor(jsg::Lock& js, ZlibModeValue mode) {
  return js.alloc<ZstdCompressionStream>(static_cast<ZlibMode>(mode), js.getExternalMemoryTarget());
}

template <typename CompressionContext>
bool ZlibUtil::ZstdCompressionStream<CompressionContext>::initialize(jsg::Lock& js,
    jsg::JsArrayBufferView params,
    jsg::JsArrayBufferView writeResult,
    jsg::Function<void()> writeCallback,
    jsg::Optional<uint64_t> pledgedSrcSize) {
  this->initializeStream(js, writeResult, kj::mv(writeCallback));

  uint64_t srcSize = pledgedSrcSize.orDefault(ZSTD_CONTENTSIZE_UNKNOWN);

  kj::Maybe<CompressionError> maybeError;
  if constexpr (CompressionContext::Mode == ZlibMode::ZSTD_ENCODE) {
    maybeError = this->context()->initialize(srcSize);
  } else {
    maybeError = this->context()->initialize();
  }

  KJ_IF_SOME(err, maybeError) {
    this->emitError(js, kj::mv(err));
    return false;
  }

  auto results = params.template asArrayPtr<int>();

  for (size_t i = 0; i < results.size(); i++) {
    if (results[i] == -1) {
      continue;
    }

    KJ_IF_SOME(err, this->context()->setParams(i, results[i])) {
      this->emitError(js, kj::mv(err));
      return false;
    }
  }
  return true;
}

template <typename CompressionContext>
jsg::Ref<ZlibUtil::BrotliCompressionStream<CompressionContext>> ZlibUtil::BrotliCompressionStream<
    CompressionContext>::constructor(jsg::Lock& js, ZlibModeValue mode) {
  return js.alloc<BrotliCompressionStream>(
      static_cast<ZlibMode>(mode), js.getExternalMemoryTarget());
}

template <typename CompressionContext>
bool ZlibUtil::BrotliCompressionStream<CompressionContext>::initialize(jsg::Lock& js,
    jsg::JsArrayBufferView params,
    jsg::JsArrayBufferView writeResult,
    jsg::Function<void()> writeCallback) {
  this->initializeStream(js, writeResult, kj::mv(writeCallback));
  auto maybeError = this->context()->initialize();

  KJ_IF_SOME(err, maybeError) {
    this->emitError(js, kj::mv(err));
    return false;
  }

  auto results = params.template asArrayPtr<uint32_t>();

  for (int i = 0; i < results.size(); i++) {
    if (results[i] == static_cast<uint32_t>(-1)) {
      continue;
    }

    KJ_IF_SOME(err, this->context()->setParams(i, results[i])) {
      this->emitError(js, kj::mv(err));
      return false;
    }
  }
  return true;
}

namespace {
template <typename Context>
static kj::Array<kj::byte> syncProcessBuffer(Context& ctx, GrowableBuffer& result) {
  do {
    result.addChunk();
    ctx.setOutputBuffer(kj::ArrayPtr(result.end(), result.available()));

    ctx.work();

    KJ_IF_SOME(error, ctx.getError()) {
      JSG_FAIL_REQUIRE(Error, error.message);
    }

    result.adjustUnused(ctx.getAvailOut());

    if (ctx.getAvailOut() == 0 && result.atMaxCapacity()) {
      // The output buffer was completely filled and has reached maxOutputLength.
      // If the stream is done, the output just happened to fit exactly — return it.
      // Otherwise the decompressed data exceeds maxOutputLength.
      JSG_REQUIRE(ctx.isStreamEnd(), RangeError, "Memory limit exceeded");
      break;
    }
  } while (ctx.getAvailOut() == 0);

  return result.releaseAsArray();
}
}  // namespace

kj::Array<kj::byte> ZlibUtil::zlibSync(
    jsg::Lock& js, ZlibUtil::InputSource data, ZlibContext::Options opts, ZlibModeValue mode) {
  // Any use of zlib APIs constitutes an implicit dependency on Allocator which must
  // remain alive until the zlib stream is destroyed
  CompressionAllocator allocator(js.getExternalMemoryTarget());
  ZlibContext ctx(allocator, static_cast<ZlibMode>(mode));

  auto chunkSize = opts.chunkSize.orDefault(ZLIB_PERFORMANT_CHUNK_SIZE);
  auto maxOutputLength = opts.maxOutputLength.orDefault(Z_MAX_CHUNK);

  // TODO(soon): Extend JSG_REQUIRE so we can pass the full level of info NodeJS provides, like the code field
  JSG_REQUIRE(Z_MIN_CHUNK <= chunkSize && chunkSize <= Z_MAX_CHUNK, RangeError,
      kj::str("The value of \"options.chunkSize\" is out of range. It must be >= ", Z_MIN_CHUNK,
          " and <= ", Z_MAX_CHUNK, ". Received ", chunkSize));
  JSG_REQUIRE(maxOutputLength >= 1 && maxOutputLength <= Z_MAX_CHUNK, RangeError,
      kj::str("The value of \"options.maxOutputLength\" is out of range. It must be >= 1 and <= ",
          Z_MAX_CHUNK, ". Received ", maxOutputLength));
  GrowableBuffer result(ZLIB_PERFORMANT_CHUNK_SIZE, maxOutputLength);

  ctx.initialize(opts.level.orDefault(Z_DEFAULT_LEVEL),
      opts.windowBits.orDefault(Z_DEFAULT_WINDOWBITS), opts.memLevel.orDefault(Z_DEFAULT_MEMLEVEL),
      opts.strategy.orDefault(Z_DEFAULT_STRATEGY), kj::mv(opts.dictionary));

  auto flush = opts.flush.orDefault(Z_NO_FLUSH);
  JSG_REQUIRE(Z_NO_FLUSH <= flush && flush <= Z_TREES, RangeError,
      kj::str("The value of \"options.flush\" is out of range. It must be >= ", Z_NO_FLUSH,
          " and <= ", Z_TREES, ". Received ", flush));

  auto finishFlush = opts.finishFlush.orDefault(Z_FINISH);
  JSG_REQUIRE(Z_NO_FLUSH <= finishFlush && finishFlush <= Z_TREES, RangeError,
      kj::str("The value of \"options.finishFlush\" is out of range. It must be >= ", Z_NO_FLUSH,
          " and <= ", Z_TREES, ". Received ", flush));

  ctx.setFlush(finishFlush);
  ctx.setInputBuffer(getInputFromSource(data));
  return syncProcessBuffer(ctx, result);
}

void ZlibUtil::zlibWithCallback(jsg::Lock& js,
    InputSource data,
    ZlibContext::Options options,
    ZlibModeValue mode,
    CompressCallback cb) {
  // Capture only relevant errors so they can be passed to the callback
  auto res = js.tryCatch([&]() {
    return CompressCallbackArg(zlibSync(js, kj::mv(data), kj::mv(options), mode));
  }, [&](jsg::Value&& exception) {
    return CompressCallbackArg(jsg::JsValue(exception.getHandle(js)));
  });

  // Ensure callback is invoked only once
  cb(js, kj::mv(res));
}

template <typename Context>
kj::Array<kj::byte> ZlibUtil::brotliSync(
    jsg::Lock& js, InputSource data, BrotliContext::Options opts) {
  // Any use of brotli APIs constitutes an implicit dependency on Allocator which must
  // remain alive until the brotli state is destroyed
  CompressionAllocator allocator(js.getExternalMemoryTarget());
  Context ctx(allocator, Context::Mode);

  auto chunkSize = opts.chunkSize.orDefault(ZLIB_PERFORMANT_CHUNK_SIZE);
  auto maxOutputLength = opts.maxOutputLength.orDefault(Z_MAX_CHUNK);

  // TODO(soon): Extend JSG_REQUIRE so we can pass the full level of info NodeJS provides, like the code field
  JSG_REQUIRE(Z_MIN_CHUNK <= chunkSize && chunkSize <= Z_MAX_CHUNK, RangeError,
      kj::str("The value of \"options.chunkSize\" is out of range. It must be >= ", Z_MIN_CHUNK,
          " and <= ", Z_MAX_CHUNK, ". Received ", chunkSize));
  JSG_REQUIRE(maxOutputLength >= 1 && maxOutputLength <= Z_MAX_CHUNK, RangeError,
      kj::str("The value of \"options.maxOutputLength\" is out of range. It must be >= 1 and <= ",
          Z_MAX_CHUNK, ". Received ", maxOutputLength));
  GrowableBuffer result(ZLIB_PERFORMANT_CHUNK_SIZE, maxOutputLength);

  KJ_IF_SOME(err, ctx.initialize()) {
    JSG_FAIL_REQUIRE(Error, err.message);
  }

  KJ_IF_SOME(params, opts.params) {
    for (const auto& field: params.fields) {
      KJ_IF_SOME(err, ctx.setParams(field.name.parseAs<int>(), field.value)) {
        JSG_FAIL_REQUIRE(Error, err.message);
      }
    }
  }

  auto flush = opts.flush.orDefault(BROTLI_OPERATION_PROCESS);
  JSG_REQUIRE(BROTLI_OPERATION_PROCESS <= flush && flush <= BROTLI_OPERATION_EMIT_METADATA,
      RangeError,
      kj::str("The value of \"options.flush\" is out of range. It must be >= ",
          BROTLI_OPERATION_PROCESS, " and <= ", BROTLI_OPERATION_EMIT_METADATA, ". Received ",
          flush));

  auto finishFlush = opts.finishFlush.orDefault(BROTLI_OPERATION_FINISH);
  JSG_REQUIRE(
      BROTLI_OPERATION_PROCESS <= finishFlush && finishFlush <= BROTLI_OPERATION_EMIT_METADATA,
      RangeError,
      kj::str("The value of \"options.finishFlush\" is out of range. It must be >= ",
          BROTLI_OPERATION_PROCESS, " and <= ", BROTLI_OPERATION_EMIT_METADATA, ". Received ",
          finishFlush));

  ctx.setFlush(finishFlush);
  ctx.setInputBuffer(getInputFromSource(data));
  return syncProcessBuffer(ctx, result);
}

template <typename Context>
void ZlibUtil::brotliWithCallback(
    jsg::Lock& js, InputSource data, BrotliContext::Options options, CompressCallback cb) {
  // Capture only relevant errors so they can be passed to the callback
  auto res = js.tryCatch([&]() {
    return CompressCallbackArg(brotliSync<Context>(js, kj::mv(data), kj::mv(options)));
  }, [&](jsg::Value&& exception) {
    return CompressCallbackArg(jsg::JsValue(exception.getHandle(js)));
  });

  // Ensure callback is invoked only once
  cb(js, kj::mv(res));
}

template <typename Context>
kj::Array<kj::byte> ZlibUtil::zstdSync(jsg::Lock& js, InputSource data, ZstdContext::Options opts) {
  Context ctx(Context::Mode);

  auto chunkSize = opts.chunkSize.orDefault(ZLIB_PERFORMANT_CHUNK_SIZE);
  auto maxOutputLength = opts.maxOutputLength.orDefault(Z_MAX_CHUNK);

  JSG_REQUIRE(Z_MIN_CHUNK <= chunkSize && chunkSize <= Z_MAX_CHUNK, RangeError,
      kj::str("The value of \"options.chunkSize\" is out of range. It must be >= ", Z_MIN_CHUNK,
          " and <= ", Z_MAX_CHUNK, ". Received ", chunkSize));
  JSG_REQUIRE(maxOutputLength >= 1 && maxOutputLength <= Z_MAX_CHUNK, RangeError,
      kj::str("The value of \"options.maxOutputLength\" is out of range. It must be >= 1 and <= ",
          Z_MAX_CHUNK, ". Received ", maxOutputLength));
  GrowableBuffer result(ZLIB_PERFORMANT_CHUNK_SIZE, maxOutputLength);

  // Initialize the context
  if constexpr (Context::Mode == ZlibMode::ZSTD_ENCODE) {
    uint64_t pledgedSrcSize = opts.pledgedSrcSize.orDefault(ZSTD_CONTENTSIZE_UNKNOWN);
    KJ_IF_SOME(err, ctx.initialize(pledgedSrcSize)) {
      JSG_FAIL_REQUIRE(Error, err.message);
    }
  } else {
    KJ_IF_SOME(err, ctx.initialize()) {
      JSG_FAIL_REQUIRE(Error, err.message);
    }
  }

  // Set parameters
  KJ_IF_SOME(params, opts.params) {
    for (const auto& field: params.fields) {
      KJ_IF_SOME(err, ctx.setParams(field.name.parseAs<int>(), field.value)) {
        JSG_FAIL_REQUIRE(Error, err.message);
      }
    }
  }

  auto flush = opts.flush.orDefault(ZSTD_e_continue);
  JSG_REQUIRE(ZSTD_e_continue <= flush && flush <= ZSTD_e_end, RangeError,
      kj::str("The value of \"options.flush\" is out of range. It must be >= ", ZSTD_e_continue,
          " and <= ", ZSTD_e_end, ". Received ", flush));

  auto finishFlush = opts.finishFlush.orDefault(ZSTD_e_end);
  JSG_REQUIRE(ZSTD_e_continue <= finishFlush && finishFlush <= ZSTD_e_end, RangeError,
      kj::str("The value of \"options.finishFlush\" is out of range. It must be >= ",
          ZSTD_e_continue, " and <= ", ZSTD_e_end, ". Received ", finishFlush));

  ctx.setFlush(finishFlush);
  ctx.setInputBuffer(getInputFromSource(data));
  return syncProcessBuffer(ctx, result);
}

template <typename Context>
void ZlibUtil::zstdWithCallback(
    jsg::Lock& js, InputSource data, ZstdContext::Options options, CompressCallback cb) {
  // Capture only relevant errors so they can be passed to the callback
  auto res = js.tryCatch([&]() {
    return CompressCallbackArg(zstdSync<Context>(js, kj::mv(data), kj::mv(options)));
  }, [&](jsg::Value&& exception) {
    return CompressCallbackArg(jsg::JsValue(exception.getHandle(js)));
  });

  // Ensure callback is invoked only once
  cb(js, kj::mv(res));
}

#ifndef CREATE_TEMPLATE
#define CREATE_TEMPLATE(T)                                                                         \
  template class ZlibUtil::CompressionStream<T>;                                                   \
  template void ZlibUtil::CompressionStream<T>::write<false>(jsg::Lock & js, int flush,            \
      jsg::Optional<jsg::JsBufferSource> input, uint32_t inputOffset, uint32_t inputLength,        \
      jsg::JsBufferSource output, uint32_t outputOffset, uint32_t outputLength);                   \
  template void ZlibUtil::CompressionStream<T>::write<true>(jsg::Lock & js, int flush,             \
      jsg::Optional<jsg::JsBufferSource> input, uint32_t inputOffset, uint32_t inputLength,        \
      jsg::JsBufferSource output, uint32_t outputOffset, uint32_t outputLength);

CREATE_TEMPLATE(ZlibContext)
CREATE_TEMPLATE(BrotliEncoderContext)
CREATE_TEMPLATE(BrotliDecoderContext)
CREATE_TEMPLATE(ZstdEncoderContext)
CREATE_TEMPLATE(ZstdDecoderContext)

template class ZlibUtil::BrotliCompressionStream<BrotliEncoderContext>;
template class ZlibUtil::BrotliCompressionStream<BrotliDecoderContext>;

template class ZlibUtil::ZstdCompressionStream<ZstdEncoderContext>;
template class ZlibUtil::ZstdCompressionStream<ZstdDecoderContext>;

template kj::Array<kj::byte> ZlibUtil::brotliSync<BrotliEncoderContext>(
    jsg::Lock& js, InputSource data, BrotliContext::Options opts);
template kj::Array<kj::byte> ZlibUtil::brotliSync<BrotliDecoderContext>(
    jsg::Lock& js, InputSource data, BrotliContext::Options opts);
template void ZlibUtil::brotliWithCallback<BrotliEncoderContext>(
    jsg::Lock& js, InputSource data, BrotliContext::Options options, CompressCallback cb);
template void ZlibUtil::brotliWithCallback<BrotliDecoderContext>(
    jsg::Lock& js, InputSource data, BrotliContext::Options options, CompressCallback cb);

template kj::Array<kj::byte> ZlibUtil::zstdSync<ZstdEncoderContext>(
    jsg::Lock& js, InputSource data, ZstdContext::Options opts);
template kj::Array<kj::byte> ZlibUtil::zstdSync<ZstdDecoderContext>(
    jsg::Lock& js, InputSource data, ZstdContext::Options opts);
template void ZlibUtil::zstdWithCallback<ZstdEncoderContext>(
    jsg::Lock& js, InputSource data, ZstdContext::Options options, CompressCallback cb);
template void ZlibUtil::zstdWithCallback<ZstdDecoderContext>(
    jsg::Lock& js, InputSource data, ZstdContext::Options options, CompressCallback cb);
#undef CREATE_TEMPLATE
#endif
}  // namespace workerd::api::node
