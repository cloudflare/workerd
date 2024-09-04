// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "util.h"
#include "zlib-util.h"

#include "nbytes.h"

// The following implementation is adapted from Node.js
// and therefore follows Node.js style as opposed to kj style.
// Latest implementation of Node.js zlib can be found at:
// https://github.com/nodejs/node/blob/main/src/node_zlib.cc
namespace workerd::api::node {

kj::ArrayPtr<kj::byte> ZlibUtil::getInputFromSource(InputSource& data) {
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
  inline explicit GrowableBuffer(size_t _chunkSize, size_t _maxCapacity) {
    auto maxChunkSize = kj::min(_chunkSize, _maxCapacity);
    builder = kj::heapArrayBuilder<kj::byte>(maxChunkSize);
    chunkSize = maxChunkSize;
    maxCapacity = _maxCapacity;
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
    JSG_ASSERT(_windowBits >= Z_MIN_WINDOWBITS && _windowBits <= Z_MAX_WINDOWBITS, Error,
        "Invalid windowBits"_kj);
  }

  JSG_REQUIRE(
      _level >= Z_MIN_LEVEL && _level <= Z_MAX_LEVEL, Error, "Invalid compression level"_kj);
  JSG_REQUIRE(
      _memLevel >= Z_MIN_MEMLEVEL && _memLevel <= Z_MAX_MEMLEVEL, Error, "Invalid memlevel"_kj);
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
    dictionary = kj::mv(dict);
  }
}

kj::Maybe<CompressionError> ZlibContext::getError() const {
  // Acceptable error states depend on the type of zlib stream.
  switch (err) {
    case Z_OK:
    case Z_BUF_ERROR:
      if (stream.avail_out != 0 && flush == Z_FINISH) {
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
      err = deflateSetDictionary(&stream, dictionary.begin(), dictionary.size());
      break;
    case ZlibMode::INFLATERAW:
      err = inflateSetDictionary(&stream, dictionary.begin(), dictionary.size());
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
  if (initialized) {
    return false;
  }
  switch (mode) {
    case ZlibMode::DEFLATE:
    case ZlibMode::GZIP:
    case ZlibMode::DEFLATERAW:
      err = deflateInit2(&stream, level, Z_DEFLATED, windowBits, memLevel, strategy);
      break;
    case ZlibMode::INFLATE:
    case ZlibMode::GUNZIP:
    case ZlibMode::INFLATERAW:
    case ZlibMode::UNZIP:
      err = inflateInit2(&stream, windowBits);
      break;
    default:
      KJ_UNREACHABLE;
  }

  if (err != Z_OK) {
    dictionary.clear();
    mode = ZlibMode::NONE;
    return true;
  }

  setDictionary();
  initialized = true;
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
      err = deflateReset(&stream);
      break;
    case ZlibMode::INFLATE:
    case ZlibMode::INFLATERAW:
    case ZlibMode::GUNZIP:
      err = inflateReset(&stream);
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

  // If the avail_out is left at 0, then it means that it ran out
  // of room.  If there was avail_out left over, then it means
  // that all the input was consumed.
  switch (mode) {
    case ZlibMode::DEFLATE:
    case ZlibMode::GZIP:
    case ZlibMode::DEFLATERAW:
      err = deflate(&stream, flush);
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
      err = inflate(&stream, flush);

      // If data was encoded with dictionary (INFLATERAW will have it set in
      // SetDictionary, don't repeat that here)
      if (mode != ZlibMode::INFLATERAW && err == Z_NEED_DICT && !dictionary.empty()) {
        // Load it
        err = inflateSetDictionary(&stream, dictionary.begin(), dictionary.size());
        if (err == Z_OK) {
          // And try to decode again
          err = inflate(&stream, flush);
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
        err = inflate(&stream, flush);
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
      err = deflateParams(&stream, _level, _strategy);
      break;
    default:
      break;
  }

  if (err != Z_OK && err != Z_BUF_ERROR) {
    return constructError("Failed to set parameters");
  }

  return kj::none;
}

void ZlibContext::close() {
  if (!initialized) {
    dictionary.clear();
    mode = ZlibMode::NONE;
    return;
  }

  auto status = Z_OK;
  switch (mode) {
    case ZlibMode::DEFLATE:
    case ZlibMode::DEFLATERAW:
    case ZlibMode::GZIP:
      status = deflateEnd(&stream);
      break;
    case ZlibMode::INFLATE:
    case ZlibMode::INFLATERAW:
    case ZlibMode::GUNZIP:
    case ZlibMode::UNZIP:
      status = inflateEnd(&stream);
      break;
    default:
      break;
  }

  JSG_REQUIRE(
      status == Z_OK || status == Z_DATA_ERROR, Error, "Uncaught error on closing zlib stream");
  mode = ZlibMode::NONE;
  dictionary.clear();
}

void ZlibContext::setBuffers(kj::ArrayPtr<kj::byte> input,
    uint32_t inputLength,
    kj::ArrayPtr<kj::byte> output,
    uint32_t outputLength) {
  stream.avail_in = inputLength;
  stream.next_in = input.begin();
  stream.avail_out = outputLength;
  stream.next_out = output.begin();
}

void ZlibContext::setInputBuffer(kj::ArrayPtr<kj::byte> input) {
  stream.next_in = input.begin();
  stream.avail_in = input.size();
}

void ZlibContext::setOutputBuffer(kj::ArrayPtr<kj::byte> output) {
  stream.next_out = output.begin();
  stream.avail_out = output.size();
}

template <typename CompressionContext>
jsg::Ref<ZlibUtil::CompressionStream<CompressionContext>> ZlibUtil::CompressionStream<
    CompressionContext>::constructor(ZlibModeValue mode) {
  return jsg::alloc<CompressionStream>(static_cast<ZlibMode>(mode));
}

template <typename CompressionContext>
ZlibUtil::CompressionStream<CompressionContext>::~CompressionStream() {
  JSG_ASSERT(!writing, Error, "Writing to compression stream"_kj);
  close();
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::emitError(
    jsg::Lock& js, const CompressionError& error) {
  KJ_IF_SOME(onError, errorHandler) {
    onError(js, error.err, kj::mv(error.code), kj::mv(error.message));
  }

  writing = false;
  if (pending_close) {
    close();
  }
}

template <typename CompressionContext>
template <bool async>
void ZlibUtil::CompressionStream<CompressionContext>::writeStream(jsg::Lock& js,
    int flush,
    kj::ArrayPtr<kj::byte> input,
    uint32_t inputLength,
    kj::ArrayPtr<kj::byte> output,
    uint32_t outputLength) {
  JSG_REQUIRE(initialized, Error, "Writing before initializing"_kj);
  JSG_REQUIRE(!closed, Error, "Already finalized"_kj);
  JSG_REQUIRE(!writing, Error, "Writing is in progress"_kj);
  JSG_REQUIRE(!pending_close, Error, "Pending close"_kj);

  writing = true;

  context()->setBuffers(input, inputLength, output, outputLength);
  context()->setFlush(flush);

  if constexpr (!async) {
    context()->work();
    if (checkError(js)) {
      updateWriteResult();
      writing = false;
    }
    return;
  }

  // On Node.js, this is called as a result of `ScheduleWork()` call.
  // Since, we implement the whole thing as sync, we're going to ahead and call the whole thing here.
  context()->work();

  // This is implemented slightly differently in Node.js
  // Node.js calls AfterThreadPoolWork().
  // Ref: https://github.com/nodejs/node/blob/9edf4a0856681a7665bd9dcf2ca7cac252784b98/src/node_zlib.cc#L402
  writing = false;
  if (!checkError(js)) return;
  updateWriteResult();
  KJ_IF_SOME(cb, writeCallback) {
    cb(js);
  }

  if (pending_close) {
    close();
  }
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::close() {
  pending_close = writing;
  if (writing) {
    return;
  }
  closed = true;
  JSG_ASSERT(initialized, Error, "Closing before initialized"_kj);
  context()->close();
}

template <typename CompressionContext>
bool ZlibUtil::CompressionStream<CompressionContext>::checkError(jsg::Lock& js) {
  KJ_IF_SOME(error, context()->getError()) {
    emitError(js, kj::mv(error));
    return false;
  }
  return true;
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::initializeStream(
    jsg::BufferSource _writeResult, jsg::Function<void()> _writeCallback) {
  writeResult = kj::mv(_writeResult);
  writeCallback = kj::mv(_writeCallback);
  initialized = true;
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::updateWriteResult() {
  KJ_IF_SOME(wr, writeResult) {
    auto ptr = wr.template asArrayPtr<uint32_t>();
    context()->getAfterWriteResult(&ptr[1], &ptr[0]);
  }
}

template <typename CompressionContext>
template <bool async>
void ZlibUtil::CompressionStream<CompressionContext>::write(jsg::Lock& js,
    int flush,
    jsg::Optional<kj::Array<kj::byte>> input,
    int inputOffset,
    int inputLength,
    kj::Array<kj::byte> output,
    int outputOffset,
    int outputLength) {
  if (flush != Z_NO_FLUSH && flush != Z_PARTIAL_FLUSH && flush != Z_SYNC_FLUSH &&
      flush != Z_FULL_FLUSH && flush != Z_FINISH && flush != Z_BLOCK) {
    JSG_FAIL_REQUIRE(Error, "Invalid flush value");
  }

  // Use default values if input is not determined
  if (input == kj::none) {
    inputLength = 0;
    inputOffset = 0;
  }

  auto input_ensured = input.map([](auto& val) { return val.asPtr(); }).orDefault({});

  JSG_REQUIRE(IsWithinBounds(inputOffset, inputLength, input_ensured.size()), Error,
      "Input access is not within bounds"_kj);
  JSG_REQUIRE(IsWithinBounds(outputOffset, outputLength, output.size()), Error,
      "Input access is not within bounds"_kj);

  writeStream<async>(js, flush, input_ensured.slice(inputOffset), inputLength,
      output.slice(outputOffset), outputLength);
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::reset(jsg::Lock& js) {
  KJ_IF_SOME(error, context()->resetStream()) {
    emitError(js, kj::mv(error));
  }
}

jsg::Ref<ZlibUtil::ZlibStream> ZlibUtil::ZlibStream::constructor(ZlibModeValue mode) {
  return jsg::alloc<ZlibStream>(static_cast<ZlibMode>(mode));
}

void ZlibUtil::ZlibStream::initialize(int windowBits,
    int level,
    int memLevel,
    int strategy,
    jsg::BufferSource writeState,
    jsg::Function<void()> writeCallback,
    jsg::Optional<kj::Array<kj::byte>> dictionary) {
  initializeStream(kj::mv(writeState), kj::mv(writeCallback));
  context()->setAllocationFunctions(AllocForZlib, FreeForZlib, this);
  context()->initialize(level, windowBits, memLevel, strategy, kj::mv(dictionary));
}

void ZlibUtil::ZlibStream::params(jsg::Lock& js, int _level, int _strategy) {
  context()->setParams(_level, _strategy);
  KJ_IF_SOME(err, context()->getError()) {
    emitError(js, kj::mv(err));
  }
}

void BrotliContext::setBuffers(kj::ArrayPtr<kj::byte> input,
    uint32_t inputLength,
    kj::ArrayPtr<kj::byte> output,
    uint32_t outputLength) {
  nextIn = reinterpret_cast<const uint8_t*>(input.begin());
  nextOut = output.begin();
  availIn = inputLength;
  availOut = outputLength;
}

void BrotliContext::setFlush(int _flush) {
  flush = static_cast<BrotliEncoderOperation>(_flush);
}

void BrotliContext::getAfterWriteResult(uint32_t* _availIn, uint32_t* _availOut) const {
  *_availIn = availIn;
  *_availOut = availOut;
}

BrotliEncoderContext::BrotliEncoderContext(ZlibMode _mode): BrotliContext(_mode) {
  auto instance = BrotliEncoderCreateInstance(alloc_brotli, free_brotli, alloc_opaque_brotli);
  state = kj::disposeWith<BrotliEncoderDestroyInstance>(instance);
}

void BrotliEncoderContext::close() {
  auto instance = BrotliEncoderCreateInstance(alloc_brotli, free_brotli, alloc_opaque_brotli);
  state = kj::disposeWith<BrotliEncoderDestroyInstance>(kj::mv(instance));
  mode = ZlibMode::NONE;
}

void BrotliEncoderContext::work() {
  JSG_REQUIRE(mode == ZlibMode::BROTLI_ENCODE, Error, "Mode should be BROTLI_ENCODE"_kj);
  JSG_REQUIRE_NONNULL(state.get(), Error, "State should not be empty"_kj);

  const uint8_t* internalNext = nextIn;
  lastResult = BrotliEncoderCompressStream(
      state.get(), flush, &availIn, &internalNext, &availOut, &nextOut, nullptr);
  nextIn += internalNext - nextIn;
}

kj::Maybe<CompressionError> BrotliEncoderContext::initialize(
    brotli_alloc_func init_alloc_func, brotli_free_func init_free_func, void* init_opaque_func) {
  alloc_brotli = init_alloc_func;
  free_brotli = init_free_func;
  alloc_opaque_brotli = init_opaque_func;

  auto instance = BrotliEncoderCreateInstance(alloc_brotli, free_brotli, alloc_opaque_brotli);
  state = kj::disposeWith<BrotliEncoderDestroyInstance>(kj::mv(instance));

  if (state.get() == nullptr) {
    return CompressionError(
        "Could not initialize Brotli instance"_kj, "ERR_ZLIB_INITIALIZATION_FAILED"_kj, -1);
  }

  return kj::none;
}

kj::Maybe<CompressionError> BrotliEncoderContext::resetStream() {
  return initialize(alloc_brotli, free_brotli, alloc_opaque_brotli);
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

kj::Array<kj::byte> syncProcessBuffer(ZlibContext& ctx, GrowableBuffer& result) {
  do {
    result.addChunk();
    ctx.setOutputBuffer(kj::ArrayPtr(result.end(), result.available()));

    ctx.work();

    KJ_IF_SOME(error, ctx.getError()) {
      JSG_FAIL_REQUIRE(Error, error.message);
    }

    result.adjustUnused(ctx.getAvailOut());
  } while (ctx.getAvailOut() == 0);

  return result.releaseAsArray();
}

// It's ZlibContext but it's RAII
class ZlibContextRAII: public ZlibContext {
public:
  using ZlibContext::ZlibContext;

  ~ZlibContextRAII() {
    close();
  }
};

kj::Array<kj::byte> ZlibUtil::zlibSync(InputSource data, Options opts, ZlibModeValue mode) {
  ZlibContextRAII ctx;

  auto chunkSize = opts.chunkSize.orDefault(ZLIB_PERFORMANT_CHUNK_SIZE);
  auto maxOutputLength = opts.maxOutputLength.orDefault(Z_MAX_CHUNK);

  JSG_REQUIRE(Z_MIN_CHUNK <= chunkSize && chunkSize <= Z_MAX_CHUNK, Error, "Invalid chunkSize");
  JSG_REQUIRE(maxOutputLength <= Z_MAX_CHUNK, Error, "Invalid maxOutputLength");
  GrowableBuffer result(ZLIB_PERFORMANT_CHUNK_SIZE, maxOutputLength);

  ctx.setMode(static_cast<ZlibMode>(mode));
  ctx.initialize(opts.level.orDefault(Z_DEFAULT_LEVEL),
      opts.windowBits.orDefault(Z_DEFAULT_WINDOWBITS), opts.memLevel.orDefault(Z_DEFAULT_MEMLEVEL),
      opts.strategy.orDefault(Z_DEFAULT_STRATEGY), kj::mv(opts.dictionary));
  ctx.setFlush(opts.finishFlush.orDefault(Z_FINISH));
  ctx.setInputBuffer(getInputFromSource(data));
  return syncProcessBuffer(ctx, result);
}

void ZlibUtil::zlibWithCallback(
    jsg::Lock& js, InputSource data, Options options, ZlibModeValue mode, CompressCallback cb) {
  try {
    cb(js, kj::none, zlibSync(kj::mv(data), kj::mv(options), mode));
  } catch (kj::Exception& ex) {
    auto tunneledError = jsg::tunneledErrorType(ex.getDescription());
    cb(js, tunneledError.message, kj::none);
  }
}

BrotliDecoderContext::BrotliDecoderContext(ZlibMode _mode): BrotliContext(_mode) {
  auto instance = BrotliDecoderCreateInstance(alloc_brotli, free_brotli, alloc_opaque_brotli);
  state = kj::disposeWith<BrotliDecoderDestroyInstance>(instance);
}

void BrotliDecoderContext::close() {
  auto instance = BrotliDecoderCreateInstance(alloc_brotli, free_brotli, alloc_opaque_brotli);
  state = kj::disposeWith<BrotliDecoderDestroyInstance>(kj::mv(instance));
  mode = ZlibMode::NONE;
}

kj::Maybe<CompressionError> BrotliDecoderContext::initialize(
    brotli_alloc_func init_alloc_func, brotli_free_func init_free_func, void* init_opaque_func) {
  alloc_brotli = init_alloc_func;
  free_brotli = init_free_func;
  alloc_opaque_brotli = init_opaque_func;

  auto instance = BrotliDecoderCreateInstance(alloc_brotli, free_brotli, alloc_opaque_brotli);
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
  return initialize(alloc_brotli, free_brotli, alloc_opaque_brotli);
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
    // Match zlib behaviour, as brotli doesn't have its own code for this.
    return CompressionError("Unexpected end of file", "Z_BUF_ERROR", Z_BUF_ERROR);
  }

  return kj::none;
}

template <typename CompressionContext>
jsg::Ref<ZlibUtil::BrotliCompressionStream<CompressionContext>> ZlibUtil::BrotliCompressionStream<
    CompressionContext>::constructor(ZlibModeValue mode) {
  return jsg::alloc<BrotliCompressionStream>(static_cast<ZlibMode>(mode));
}

template <typename CompressionContext>
bool ZlibUtil::BrotliCompressionStream<CompressionContext>::initialize(jsg::Lock& js,
    jsg::BufferSource params,
    jsg::BufferSource writeResult,
    jsg::Function<void()> writeCallback) {
  this->initializeStream(kj::mv(writeResult), kj::mv(writeCallback));
  auto maybeError =
      this->context()->initialize(CompressionStream<CompressionContext>::AllocForBrotli,
          CompressionStream<CompressionContext>::FreeForZlib,
          static_cast<CompressionStream<CompressionContext>*>(this));

  KJ_IF_SOME(err, maybeError) {
    this->emitError(js, kj::mv(err));
    return false;
  }

  auto results = params.template asArrayPtr<uint32_t>();

  for (int i = 0; i < results.size(); i++) {
    if (results[i] == static_cast<uint32_t>(-1)) {
      continue;
    }

    maybeError = this->context()->setParams(i, results[i]);
    KJ_IF_SOME(err, maybeError) {
      this->emitError(js, kj::mv(err));
      return false;
    }
  }
  return true;
}

template <typename CompressionContext>
void* ZlibUtil::CompressionStream<CompressionContext>::AllocForZlib(
    void* data, uInt items, uInt size) {
  size_t real_size =
      nbytes::MultiplyWithOverflowCheck(static_cast<size_t>(items), static_cast<size_t>(size));
  return AllocForBrotli(data, real_size);
}

template <typename CompressionContext>
void* ZlibUtil::CompressionStream<CompressionContext>::AllocForBrotli(void* data, size_t size) {
  size += sizeof(size_t);
  auto* ctx = static_cast<CompressionStream*>(data);
  auto memory = kj::heapArray<uint8_t>(size);
  auto begin = memory.begin();
  // TODO(soon): Check if we need to store the size of the block in the pointer like Node.js
  *reinterpret_cast<size_t*>(begin) = size;
  ctx->allocations.insert(begin, kj::mv(memory));
  return begin + sizeof(size_t);
}

template <typename CompressionContext>
void ZlibUtil::CompressionStream<CompressionContext>::FreeForZlib(void* data, void* pointer) {
  if (KJ_UNLIKELY(pointer == nullptr)) return;
  auto* ctx = static_cast<CompressionStream*>(data);
  auto real_pointer = static_cast<uint8_t*>(pointer) - sizeof(size_t);
  JSG_REQUIRE(ctx->allocations.erase(real_pointer), Error, "Zlib allocation should exist"_kj);
}

#ifndef CREATE_TEMPLATE
#define CREATE_TEMPLATE(T)                                                                         \
  template void* ZlibUtil::CompressionStream<T>::AllocForZlib(void* data, uInt items, uInt size);  \
  template void* ZlibUtil::CompressionStream<T>::AllocForBrotli(void* data, size_t size);          \
  template void ZlibUtil::CompressionStream<T>::FreeForZlib(void* data, void* pointer);            \
  template void ZlibUtil::CompressionStream<T>::reset(jsg::Lock& js);                              \
  template void ZlibUtil::CompressionStream<T>::write<false>(jsg::Lock & js, int flush,            \
      jsg::Optional<kj::Array<kj::byte>> input, int inputOffset, int inputLength,                  \
      kj::Array<kj::byte> output, int outputOffset, int outputLength);                             \
  template void ZlibUtil::CompressionStream<T>::write<true>(jsg::Lock & js, int flush,             \
      jsg::Optional<kj::Array<kj::byte>> input, int inputOffset, int inputLength,                  \
      kj::Array<kj::byte> output, int outputOffset, int outputLength);                             \
  template jsg::Ref<ZlibUtil::CompressionStream<T>> ZlibUtil::CompressionStream<T>::constructor(   \
      ZlibModeValue mode);

CREATE_TEMPLATE(ZlibContext)
CREATE_TEMPLATE(BrotliEncoderContext)
CREATE_TEMPLATE(BrotliDecoderContext)

template jsg::Ref<ZlibUtil::BrotliCompressionStream<BrotliEncoderContext>> ZlibUtil::
    BrotliCompressionStream<BrotliEncoderContext>::constructor(ZlibModeValue mode);
template jsg::Ref<ZlibUtil::BrotliCompressionStream<BrotliDecoderContext>> ZlibUtil::
    BrotliCompressionStream<BrotliDecoderContext>::constructor(ZlibModeValue mode);
template bool ZlibUtil::BrotliCompressionStream<BrotliEncoderContext>::initialize(
    jsg::Lock&, jsg::BufferSource, jsg::BufferSource, jsg::Function<void()>);
template bool ZlibUtil::BrotliCompressionStream<BrotliDecoderContext>::initialize(
    jsg::Lock&, jsg::BufferSource, jsg::BufferSource, jsg::Function<void()>);

#undef CREATE_TEMPLATE
#endif
}  // namespace workerd::api::node
