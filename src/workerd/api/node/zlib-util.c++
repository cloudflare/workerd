// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "zlib-util.h"

namespace workerd::api::node {

uint32_t ZlibUtil::crc32Sync(kj::Array<kj::byte> data, uint32_t value) {
  // Note: Bytef is defined in zlib.h
  return crc32(value, reinterpret_cast<const Bytef*>(data.begin()), data.size());
}

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
      if (dictionary.empty())
        return constructError("Missing dictionary"_kj);
      else
        return constructError("Bad dictionary"_kj);
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
      ;
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

jsg::Ref<ZlibUtil::ZlibStream> ZlibUtil::ZlibStream::constructor(ZlibModeValue mode) {
  return jsg::alloc<ZlibStream>(static_cast<ZlibMode>(mode));
}

template <typename CompressionContext>
CompressionStream<CompressionContext>::~CompressionStream() noexcept(false) {
  JSG_ASSERT(!writing, Error, "Writing to compression stream"_kj);
  close();
}

template <typename CompressionContext>
void CompressionStream<CompressionContext>::emitError(
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
void CompressionStream<CompressionContext>::writeStream(jsg::Lock& js,
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

  context.setBuffers(input, inputLength, output, outputLength);
  context.setFlush(flush);

  if constexpr (!async) {
    context.work();
    if (checkError(js)) {
      updateWriteResult();
      writing = false;
    }
    return;
  }

  // On Node.js, this is called as a result of `ScheduleWork()` call.
  // Since, we implement the whole thing as sync, we're going to ahead and call the whole thing here.
  context.work();

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
void CompressionStream<CompressionContext>::close() {
  pending_close = writing;
  if (writing) {
    return;
  }
  closed = true;
  JSG_ASSERT(initialized, Error, "Closing before initialized"_kj);
  context.close();
}

template <typename CompressionContext>
bool CompressionStream<CompressionContext>::checkError(jsg::Lock& js) {
  KJ_IF_SOME(error, context.getError()) {
    emitError(js, kj::mv(error));
    return false;
  }
  return true;
}

template <typename CompressionContext>
void CompressionStream<CompressionContext>::initializeStream(
    jsg::BufferSource _writeResult, jsg::Function<void()> _writeCallback) {
  writeResult = kj::mv(_writeResult);
  writeCallback = kj::mv(_writeCallback);
  initialized = true;
}

template <typename CompressionContext>
void CompressionStream<CompressionContext>::updateWriteResult() {
  KJ_IF_SOME(wr, writeResult) {
    auto ptr = wr.template asArrayPtr<uint32_t>();
    context.getAfterWriteResult(&ptr[1], &ptr[0]);
  }
}

ZlibUtil::ZlibStream::ZlibStream(ZlibMode mode): CompressionStream() {
  context.setMode(mode);
}

void ZlibUtil::ZlibStream::initialize(int windowBits,
    int level,
    int memLevel,
    int strategy,
    jsg::BufferSource writeState,
    jsg::Function<void()> writeCallback,
    jsg::Optional<kj::Array<kj::byte>> dictionary) {
  initializeStream(kj::mv(writeState), kj::mv(writeCallback));
  context.initialize(level, windowBits, memLevel, strategy, kj::mv(dictionary));
}

template <bool async = false>
void ZlibUtil::ZlibStream::write_(jsg::Lock& js,
    int flush,
    jsg::Optional<kj::Array<kj::byte>> input,
    int inputOffset,
    int inputLength,
    kj::ArrayPtr<kj::byte> output,
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

  JSG_REQUIRE((inputLength > inputOffset) || inputLength == 0, Error,
      kj::str("Input offset should be smaller or equal to length, but received offset: ",
          inputOffset, " and length: ", inputLength));
  JSG_REQUIRE((outputLength > outputOffset) || outputLength == 0, Error,
      kj::str("Output offset should be smaller or equal to length, but received offset: ",
          outputOffset, " and length: ", outputLength));

  auto input_ensured = input.map([](auto& val) { return val.asPtr(); }).orDefault({});
  writeStream<async>(js, flush, input_ensured.slice(inputOffset), inputLength,
      output.slice(outputOffset), outputLength);
}

void ZlibUtil::ZlibStream::write(jsg::Lock& js,
    int flush,
    jsg::Optional<kj::Array<kj::byte>> input,
    int inputOffset,
    int inputLength,
    kj::Array<kj::byte> output,
    int outputOffset,
    int outputLength) {
  write_<true>(js, flush, kj::mv(input), inputOffset, inputLength, output.asPtr(), outputOffset,
      outputLength);
}

void ZlibUtil::ZlibStream::writeSync(jsg::Lock& js,
    int flush,
    jsg::Optional<kj::Array<kj::byte>> input,
    int inputOffset,
    int inputLength,
    kj::Array<kj::byte> output,
    int outputOffset,
    int outputLength) {
  write_<false>(js, flush, kj::mv(input), inputOffset, inputLength, output.asPtr(), outputOffset,
      outputLength);
}

void ZlibUtil::ZlibStream::params(jsg::Lock& js, int _level, int _strategy) {
  context.setParams(_level, _strategy);
  KJ_IF_SOME(err, context.getError()) {
    emitError(js, kj::mv(err));
  }
}

void ZlibUtil::ZlibStream::reset(jsg::Lock& js) {
  KJ_IF_SOME(error, context.resetStream()) {
    emitError(js, kj::mv(error));
  }
}

}  // namespace workerd::api::node
