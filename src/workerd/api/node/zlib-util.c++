// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "zlib-util.h"
#include "util.h"

#include "nbytes.h"

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

ZlibContext::~ZlibContext() noexcept(false) {
  if (!initialized) {
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

void ZlibContext::setInputBuffer(kj::ArrayPtr<const kj::byte> input) {
  // The define Z_CONST is not set, so zlib always takes mutable pointers
  stream.next_in = const_cast<kj::byte*>(input.begin());
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
  // Context is closed on the destructor of the CompressionContext.
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
  context()->setAllocationFunctions(Allocator::AllocForZlib, Allocator::FreeForZlib, &allocator);
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

void BrotliContext::setInputBuffer(kj::ArrayPtr<const kj::byte> input) {
  nextIn = input.begin();
  availIn = input.size();
}

void BrotliContext::setOutputBuffer(kj::ArrayPtr<kj::byte> output) {
  nextOut = output.begin();
  availOut = output.size();
}

kj::uint BrotliContext::getAvailOut() const {
  return availOut;
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

BrotliDecoderContext::BrotliDecoderContext(ZlibMode _mode): BrotliContext(_mode) {
  auto instance = BrotliDecoderCreateInstance(alloc_brotli, free_brotli, alloc_opaque_brotli);
  state = kj::disposeWith<BrotliDecoderDestroyInstance>(instance);
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
  auto maybeError = this->context()->initialize(
      Allocator::AllocForBrotli, Allocator::FreeForZlib, &this->allocator);

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

void* ZlibUtil::Allocator::AllocForZlib(void* data, uInt items, uInt size) {
  size_t real_size =
      nbytes::MultiplyWithOverflowCheck(static_cast<size_t>(items), static_cast<size_t>(size));
  return AllocForBrotli(data, real_size);
}

void* ZlibUtil::Allocator::AllocForBrotli(void* opaque, size_t size) {
  auto* thisAllocator = static_cast<Allocator*>(opaque);
  auto memory = kj::heapArray<uint8_t>(size);
  auto begin = memory.begin();
  thisAllocator->allocations.insert(begin, kj::mv(memory));
  return begin;
}

void ZlibUtil::Allocator::FreeForZlib(void* opaque, void* pointer) {
  if (KJ_UNLIKELY(pointer == nullptr)) return;
  auto* thisAllocator = static_cast<Allocator*>(opaque);
  JSG_REQUIRE(thisAllocator->allocations.erase(pointer), Error, "Zlib allocation should exist"_kj);
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
  } while (ctx.getAvailOut() == 0);

  return result.releaseAsArray();
}
}  // namespace

kj::Array<kj::byte> ZlibUtil::zlibSync(
    ZlibUtil::InputSource data, ZlibContext::Options opts, ZlibModeValue mode) {
  // Any use of zlib APIs consistutes an implicit dependency on Allocator which must remain alive until the zlib stream is destroyed
  Allocator allocator;
  ZlibContext ctx(static_cast<ZlibMode>(mode));
  ctx.setAllocationFunctions(Allocator::AllocForZlib, Allocator::FreeForZlib, &allocator);

  auto chunkSize = opts.chunkSize.orDefault(ZLIB_PERFORMANT_CHUNK_SIZE);
  auto maxOutputLength = opts.maxOutputLength.orDefault(Z_MAX_CHUNK);

  // TODO(soon): Extend JSG_REQUIRE so we can pass the full level of info NodeJS provides, like the code field
  JSG_REQUIRE(Z_MIN_CHUNK <= chunkSize && chunkSize <= Z_MAX_CHUNK, RangeError,
      kj::str("The value of \"options.chunkSize\" is out of range. It must be >= ", Z_MIN_CHUNK,
          " and <= ", Z_MAX_CHUNK, ". Received ", chunkSize));
  JSG_REQUIRE(maxOutputLength <= Z_MAX_CHUNK, RangeError, "Invalid maxOutputLength"_kj);
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
    return CompressCallbackArg(zlibSync(kj::mv(data), kj::mv(options), mode));
  }, [&](jsg::Value&& exception) {
    return CompressCallbackArg(jsg::JsValue(exception.getHandle(js)));
  });

  // Ensure callback is invoked only once
  cb(js, kj::mv(res));
}

template <typename Context>
kj::Array<kj::byte> ZlibUtil::brotliSync(InputSource data, BrotliContext::Options opts) {
  // Any use of brotli APIs consistutes an implicit dependency on Allocator which must remain alive until the brotli state is destroyed
  Allocator allocator;
  Context ctx(Context::Mode);

  auto chunkSize = opts.chunkSize.orDefault(ZLIB_PERFORMANT_CHUNK_SIZE);
  auto maxOutputLength = opts.maxOutputLength.orDefault(Z_MAX_CHUNK);

  // TODO(soon): Extend JSG_REQUIRE so we can pass the full level of info NodeJS provides, like the code field
  JSG_REQUIRE(Z_MIN_CHUNK <= chunkSize && chunkSize <= Z_MAX_CHUNK, RangeError,
      kj::str("The value of \"options.chunkSize\" is out of range. It must be >= ", Z_MIN_CHUNK,
          " and <= ", Z_MAX_CHUNK, ". Received ", chunkSize));
  JSG_REQUIRE(maxOutputLength <= Z_MAX_CHUNK, Error, "Invalid maxOutputLength"_kj);
  GrowableBuffer result(ZLIB_PERFORMANT_CHUNK_SIZE, maxOutputLength);

  KJ_IF_SOME(err, ctx.initialize(Allocator::AllocForBrotli, Allocator::FreeForZlib, &allocator)) {
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
    return CompressCallbackArg(brotliSync<Context>(kj::mv(data), kj::mv(options)));
  }, [&](jsg::Value&& exception) {
    return CompressCallbackArg(jsg::JsValue(exception.getHandle(js)));
  });

  // Ensure callback is invoked only once
  cb(js, kj::mv(res));
}

#ifndef CREATE_TEMPLATE
#define CREATE_TEMPLATE(T)                                                                         \
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

template kj::Array<kj::byte> ZlibUtil::brotliSync<BrotliEncoderContext>(
    InputSource data, BrotliContext::Options opts);
template kj::Array<kj::byte> ZlibUtil::brotliSync<BrotliDecoderContext>(
    InputSource data, BrotliContext::Options opts);
template void ZlibUtil::brotliWithCallback<BrotliEncoderContext>(
    jsg::Lock& js, InputSource data, BrotliContext::Options options, CompressCallback cb);
template void ZlibUtil::brotliWithCallback<BrotliDecoderContext>(
    jsg::Lock& js, InputSource data, BrotliContext::Options options, CompressCallback cb);
#undef CREATE_TEMPLATE
#endif
}  // namespace workerd::api::node
