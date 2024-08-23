// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/exception.h>
#include <workerd/jsg/buffersource.h>
#include <zlib.h>

#include <kj/array.h>
#include <kj/compat/brotli.h>
#include <kj/vector.h>

#include <cstdlib>

namespace workerd::api::node {

#ifndef ZLIB_ERROR_CODES
#define ZLIB_ERROR_CODES(V)                                                                        \
  V(Z_OK)                                                                                          \
  V(Z_STREAM_END)                                                                                  \
  V(Z_NEED_DICT)                                                                                   \
  V(Z_ERRNO)                                                                                       \
  V(Z_STREAM_ERROR)                                                                                \
  V(Z_DATA_ERROR)                                                                                  \
  V(Z_MEM_ERROR)                                                                                   \
  V(Z_BUF_ERROR)                                                                                   \
  V(Z_VERSION_ERROR)

inline const char* ZlibStrerror(int err) {
#define V(code)                                                                                    \
  if (err == code) return #code;
  ZLIB_ERROR_CODES(V)
#undef V
  return "Z_UNKNOWN_ERROR";
}
#endif  // ZLIB_ERROR_CODES

// Certain zlib constants are defined by Node.js itself
static constexpr auto Z_MIN_CHUNK = 64;
static constexpr auto Z_MAX_CHUNK = 128 * 1024 * 1024;
static constexpr auto Z_DEFAULT_CHUNK = 16 * 1024;
static constexpr auto Z_MIN_MEMLEVEL = 1;

static constexpr auto Z_MAX_MEMLEVEL = 9;
static constexpr auto Z_DEFAULT_MEMLEVEL = 8;
static constexpr auto Z_MIN_LEVEL = -1;
static constexpr auto Z_MAX_LEVEL = 9;
static constexpr auto Z_DEFAULT_LEVEL = Z_DEFAULT_COMPRESSION;
static constexpr auto Z_MIN_WINDOWBITS = 8;
static constexpr auto Z_MAX_WINDOWBITS = 15;
static constexpr auto Z_DEFAULT_WINDOWBITS = 15;

static constexpr uint8_t GZIP_HEADER_ID1 = 0x1f;
static constexpr uint8_t GZIP_HEADER_ID2 = 0x8b;

using ZlibModeValue = uint8_t;
enum class ZlibMode : ZlibModeValue {
  NONE,
  DEFLATE,
  INFLATE,
  GZIP,
  GUNZIP,
  DEFLATERAW,
  INFLATERAW,
  UNZIP,
  BROTLI_DECODE,
  BROTLI_ENCODE
};

struct CompressionError {
  CompressionError(kj::StringPtr _message, kj::StringPtr _code, int _err)
      : message(kj::str(_message)),
        code(kj::str(_code)),
        err(_err) {
    JSG_REQUIRE(message.size() != 0, Error, "Compression error message should not be null");
  }

  kj::String message;
  kj::String code;
  int err;
};

class ZlibContext final {
public:
  ZlibContext() = default;

  KJ_DISALLOW_COPY(ZlibContext);

  void close();
  void setBuffers(kj::ArrayPtr<kj::byte> input,
      uint32_t inputLength,
      kj::ArrayPtr<kj::byte> output,
      uint32_t outputLength);
  int getFlush() const {
    return flush;
  };
  void setFlush(int value) {
    flush = value;
  };
  // Function signature is same as Node.js implementation.
  // Ref: https://github.com/nodejs/node/blob/9edf4a0856681a7665bd9dcf2ca7cac252784b98/src/node_zlib.cc#L880
  void getAfterWriteResult(uint32_t* availIn, uint32_t* availOut) const {
    *availIn = stream.avail_in;
    *availOut = stream.avail_out;
  }
  void setMode(ZlibMode value) {
    mode = value;
  };
  kj::Maybe<CompressionError> resetStream();
  kj::Maybe<CompressionError> getError() const;

  // Equivalent to Node.js' `DoThreadPoolWork` function.
  // Ref: https://github.com/nodejs/node/blob/9edf4a0856681a7665bd9dcf2ca7cac252784b98/src/node_zlib.cc#L760
  void work();

  uint getAvailIn() const {
    return stream.avail_in;
  };
  void setAvailIn(uint value) {
    stream.avail_in = value;
  };
  uint getAvailOut() const {
    return stream.avail_out;
  }
  void setAvailOut(uint value) {
    stream.avail_out = value;
  };

  // Zlib
  void initialize(int _level,
      int _windowBits,
      int _memLevel,
      int _strategy,
      jsg::Optional<kj::Array<kj::byte>> _dictionary);
  kj::Maybe<CompressionError> setParams(int level, int strategy);

private:
  bool initializeZlib();
  kj::Maybe<CompressionError> setDictionary();

  CompressionError constructError(kj::StringPtr message) const {
    if (stream.msg != nullptr) message = kj::StringPtr(stream.msg);

    return {kj::str(message), kj::str(ZlibStrerror(err)), err};
  };

  bool initialized = false;
  ZlibMode mode = ZlibMode::NONE;
  int flush = Z_NO_FLUSH;
  int windowBits = 0;
  int level = 0;
  int memLevel = 0;
  int strategy = 0;
  kj::Vector<kj::byte> dictionary{};

  int err = Z_OK;
  unsigned int gzip_id_bytes_read = 0;
  z_stream stream{};
};

using CompressionStreamErrorHandler = jsg::Function<void(int, kj::StringPtr, kj::StringPtr)>;

template <typename CompressionContext>
class CompressionStream {
public:
  CompressionStream() = default;
  ~CompressionStream() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(CompressionStream);

  void close();
  bool checkError(jsg::Lock& js);
  void emitError(jsg::Lock& js, const CompressionError& error);
  template <bool async>
  void writeStream(jsg::Lock& js,
      int flush,
      kj::ArrayPtr<kj::byte> input,
      uint32_t inputLength,
      kj::ArrayPtr<kj::byte> output,
      uint32_t outputLength);
  void setErrorHandler(CompressionStreamErrorHandler handler) {
    errorHandler = kj::mv(handler);
  };
  void initializeStream(jsg::BufferSource _write_result, jsg::Function<void()> writeCallback);
  void updateWriteResult();

protected:
  CompressionContext context;

private:
  bool initialized = false;
  bool writing = false;
  bool pending_close = false;
  bool closed = false;

  // Equivalent to `write_js_callback` in Node.js
  jsg::Optional<jsg::Function<void()>> writeCallback;
  jsg::Optional<jsg::BufferSource> writeResult;
  jsg::Optional<CompressionStreamErrorHandler> errorHandler;
};

// Implements utilities in support of the Node.js Zlib
class ZlibUtil final: public jsg::Object {
public:
  ZlibUtil() = default;
  ZlibUtil(jsg::Lock&, const jsg::Url&) {}

  class ZlibStream final: public jsg::Object, public CompressionStream<ZlibContext> {
  public:
    ZlibStream(ZlibMode mode);
    KJ_DISALLOW_COPY_AND_MOVE(ZlibStream);
    static jsg::Ref<ZlibStream> constructor(ZlibModeValue mode);

    // Instance methods
    void initialize(int windowBits,
        int level,
        int memLevel,
        int strategy,
        jsg::BufferSource writeState,
        jsg::Function<void()> writeCallback,
        jsg::Optional<kj::Array<kj::byte>> dictionary);
    template <bool async>
    void write_(jsg::Lock& js,
        int flush,
        jsg::Optional<kj::Array<kj::byte>> input,
        int inputOffset,
        int inputLength,
        kj::ArrayPtr<kj::byte> output,
        int outputOffset,
        int outputLength);

    // TODO(soon): Find a way to expose functions with templates using JSG_METHOD.
    void write(jsg::Lock& js,
        int flush,
        jsg::Optional<kj::Array<kj::byte>> input,
        int inputOffset,
        int inputLength,
        kj::Array<kj::byte> output,
        int outputOffset,
        int outputLength);
    void writeSync(jsg::Lock& js,
        int flush,
        jsg::Optional<kj::Array<kj::byte>> input,
        int inputOffset,
        int inputLength,
        kj::Array<kj::byte> output,
        int outputOffset,
        int outputLength);
    void params(jsg::Lock& js, int level, int strategy);
    void reset(jsg::Lock& js);

    JSG_RESOURCE_TYPE(ZlibStream) {
      JSG_METHOD(initialize);
      JSG_METHOD(close);
      JSG_METHOD(write);
      JSG_METHOD(writeSync);
      JSG_METHOD(params);
      JSG_METHOD(setErrorHandler);
      JSG_METHOD(reset);
    }
  };

  uint32_t crc32Sync(kj::Array<kj::byte> data, uint32_t value);

  JSG_RESOURCE_TYPE(ZlibUtil) {
    JSG_METHOD_NAMED(crc32, crc32Sync);
    JSG_NESTED_TYPE(ZlibStream);

    // zlib.constants (part of the API contract for node:zlib)
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_NO_FLUSH, Z_NO_FLUSH);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_PARTIAL_FLUSH, Z_PARTIAL_FLUSH);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_SYNC_FLUSH, Z_SYNC_FLUSH);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_FULL_FLUSH, Z_FULL_FLUSH);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_FINISH, Z_FINISH);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_BLOCK, Z_BLOCK);

    JSG_STATIC_CONSTANT_NAMED(CONST_Z_OK, Z_OK);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_STREAM_END, Z_STREAM_END);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_NEED_DICT, Z_NEED_DICT);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_ERRNO, Z_ERRNO);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_STREAM_ERROR, Z_STREAM_ERROR);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_DATA_ERROR, Z_DATA_ERROR);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MEM_ERROR, Z_MEM_ERROR);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_BUF_ERROR, Z_BUF_ERROR);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_VERSION_ERROR, Z_VERSION_ERROR);

    JSG_STATIC_CONSTANT_NAMED(CONST_Z_NO_COMPRESSION, Z_NO_COMPRESSION);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_BEST_SPEED, Z_BEST_SPEED);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_BEST_COMPRESSION, Z_BEST_COMPRESSION);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_DEFAULT_COMPRESSION, Z_DEFAULT_COMPRESSION);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_FILTERED, Z_FILTERED);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_HUFFMAN_ONLY, Z_HUFFMAN_ONLY);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_RLE, Z_RLE);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_FIXED, Z_FIXED);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_DEFAULT_STRATEGY, Z_DEFAULT_STRATEGY);
    JSG_STATIC_CONSTANT_NAMED(CONST_ZLIB_VERNUM, ZLIB_VERNUM);

    JSG_STATIC_CONSTANT_NAMED(CONST_DEFLATE, static_cast<ZlibModeValue>(ZlibMode::DEFLATE));
    JSG_STATIC_CONSTANT_NAMED(CONST_INFLATE, static_cast<ZlibModeValue>(ZlibMode::INFLATE));
    JSG_STATIC_CONSTANT_NAMED(CONST_GZIP, static_cast<ZlibModeValue>(ZlibMode::GZIP));
    JSG_STATIC_CONSTANT_NAMED(CONST_GUNZIP, static_cast<ZlibModeValue>(ZlibMode::GUNZIP));
    JSG_STATIC_CONSTANT_NAMED(CONST_DEFLATERAW, static_cast<ZlibModeValue>(ZlibMode::DEFLATERAW));
    JSG_STATIC_CONSTANT_NAMED(CONST_INFLATERAW, static_cast<ZlibModeValue>(ZlibMode::INFLATERAW));
    JSG_STATIC_CONSTANT_NAMED(CONST_UNZIP, static_cast<ZlibModeValue>(ZlibMode::UNZIP));
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODE, static_cast<ZlibModeValue>(ZlibMode::BROTLI_DECODE));
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_ENCODE, static_cast<ZlibModeValue>(ZlibMode::BROTLI_ENCODE));
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MIN_WINDOWBITS, Z_MIN_WINDOWBITS);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MAX_WINDOWBITS, Z_MAX_WINDOWBITS);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_DEFAULT_WINDOWBITS, Z_DEFAULT_WINDOWBITS);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MIN_CHUNK, Z_MIN_CHUNK);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MAX_CHUNK, Z_MAX_CHUNK);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_DEFAULT_CHUNK, Z_DEFAULT_CHUNK);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MIN_MEMLEVEL, Z_MIN_MEMLEVEL);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MAX_MEMLEVEL, Z_MAX_MEMLEVEL);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_DEFAULT_MEMLEVEL, Z_DEFAULT_MEMLEVEL);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MIN_LEVEL, Z_MIN_LEVEL);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_MAX_LEVEL, Z_MAX_LEVEL);
    JSG_STATIC_CONSTANT_NAMED(CONST_Z_DEFAULT_LEVEL, Z_DEFAULT_LEVEL);

    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_OPERATION_PROCESS, BROTLI_OPERATION_PROCESS);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_OPERATION_FLUSH, BROTLI_OPERATION_FLUSH);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_OPERATION_FINISH, BROTLI_OPERATION_FINISH);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_OPERATION_EMIT_METADATA, BROTLI_OPERATION_EMIT_METADATA);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_MODE, BROTLI_PARAM_MODE);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MODE_GENERIC, BROTLI_MODE_GENERIC);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MODE_TEXT, BROTLI_MODE_TEXT);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MODE_FONT, BROTLI_MODE_FONT);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DEFAULT_MODE, BROTLI_DEFAULT_MODE);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_QUALITY, BROTLI_PARAM_QUALITY);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MIN_QUALITY, BROTLI_MIN_QUALITY);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MAX_QUALITY, BROTLI_MAX_QUALITY);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_QUALITY);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_LGWIN, BROTLI_PARAM_LGWIN);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MIN_WINDOW_BITS, BROTLI_MIN_WINDOW_BITS);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MAX_WINDOW_BITS, BROTLI_MAX_WINDOW_BITS);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_LARGE_MAX_WINDOW_BITS, BROTLI_LARGE_MAX_WINDOW_BITS);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_WINDOW);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_LGBLOCK, BROTLI_PARAM_LGBLOCK);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MIN_INPUT_BLOCK_BITS, BROTLI_MIN_INPUT_BLOCK_BITS);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_MAX_INPUT_BLOCK_BITS, BROTLI_MAX_INPUT_BLOCK_BITS);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING,
        BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_SIZE_HINT, BROTLI_PARAM_SIZE_HINT);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_LARGE_WINDOW, BROTLI_PARAM_LARGE_WINDOW);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_NPOSTFIX, BROTLI_PARAM_NPOSTFIX);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_PARAM_NDIRECT, BROTLI_PARAM_NDIRECT);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_RESULT_ERROR, BROTLI_DECODER_RESULT_ERROR);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_RESULT_SUCCESS, BROTLI_DECODER_RESULT_SUCCESS);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT, BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT, BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION,
        BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_PARAM_LARGE_WINDOW, BROTLI_DECODER_PARAM_LARGE_WINDOW);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_NO_ERROR, BROTLI_DECODER_NO_ERROR);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_SUCCESS, BROTLI_DECODER_SUCCESS);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_NEEDS_MORE_INPUT, BROTLI_DECODER_NEEDS_MORE_INPUT);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_NEEDS_MORE_OUTPUT, BROTLI_DECODER_NEEDS_MORE_OUTPUT);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE,
        BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_RESERVED, BROTLI_DECODER_ERROR_FORMAT_RESERVED);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE,
        BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET,
        BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME,
        BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_CL_SPACE, BROTLI_DECODER_ERROR_FORMAT_CL_SPACE);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE, BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT,
        BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1,
        BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2,
        BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_TRANSFORM, BROTLI_DECODER_ERROR_FORMAT_TRANSFORM);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_DICTIONARY, BROTLI_DECODER_ERROR_FORMAT_DICTIONARY);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS, BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_PADDING_1, BROTLI_DECODER_ERROR_FORMAT_PADDING_1);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_PADDING_2, BROTLI_DECODER_ERROR_FORMAT_PADDING_2);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_FORMAT_DISTANCE, BROTLI_DECODER_ERROR_FORMAT_DISTANCE);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET, BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_INVALID_ARGUMENTS, BROTLI_DECODER_ERROR_INVALID_ARGUMENTS);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES, BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS, BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP, BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1, BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2, BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2);
    JSG_STATIC_CONSTANT_NAMED(CONST_BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES,
        BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES);
    JSG_STATIC_CONSTANT_NAMED(
        CONST_BROTLI_DECODER_ERROR_UNREACHABLE, BROTLI_DECODER_ERROR_UNREACHABLE);
  };
};

#define EW_NODE_ZLIB_ISOLATE_TYPES api::node::ZlibUtil, api::node::ZlibUtil::ZlibStream

}  // namespace workerd::api::node
