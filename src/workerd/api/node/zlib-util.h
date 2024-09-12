// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.
#pragma once

#include <workerd/jsg/jsg.h>

#include <zlib.h>

#include <kj/array.h>
#include <kj/one-of.h>
#include <kj/vector.h>

#include <brotli/decode.h>
#include <brotli/encode.h>

// The following implementation is adapted from Node.js
// and therefore follows Node.js style as opposed to kj style.
// Latest implementation of Node.js zlib can be found at:
// https://github.com/nodejs/node/blob/main/src/node_zlib.cc
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

// When possible, we intentionally override chunkSize to a value that is likely to perform better
static constexpr auto ZLIB_PERFORMANT_CHUNK_SIZE = 40 * 1024;

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
  explicit ZlibContext(ZlibMode _mode): mode(_mode) {}
  ZlibContext() = default;
  ~ZlibContext() noexcept(false);

  KJ_DISALLOW_COPY_AND_MOVE(ZlibContext);

  void setBuffers(kj::ArrayPtr<kj::byte> input,
      uint32_t inputLength,
      kj::ArrayPtr<kj::byte> output,
      uint32_t outputLength);

  void setInputBuffer(kj::ArrayPtr<const kj::byte> input);
  void setOutputBuffer(kj::ArrayPtr<kj::byte> output);

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
  void setAllocationFunctions(alloc_func alloc, free_func free, void* opaque) {
    stream.zalloc = alloc;
    stream.zfree = free;
    stream.opaque = opaque;
  }

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
  struct Options {
    jsg::Optional<int> flush;
    jsg::Optional<int> finishFlush;
    jsg::Optional<uint> chunkSize;
    jsg::Optional<kj::uint> windowBits;
    jsg::Optional<int> level;
    jsg::Optional<kj::uint> memLevel;
    jsg::Optional<kj::uint> strategy;
    jsg::Optional<kj::Array<kj::byte>> dictionary;
    jsg::Optional<kj::uint> maxOutputLength;

    JSG_STRUCT(flush,
        finishFlush,
        chunkSize,
        windowBits,
        level,
        memLevel,
        strategy,
        dictionary,
        maxOutputLength);
  };

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

class BrotliContext {
public:
  explicit BrotliContext(ZlibMode _mode): mode(_mode) {}
  KJ_DISALLOW_COPY(BrotliContext);
  void setBuffers(kj::ArrayPtr<kj::byte> input,
      uint32_t inputLength,
      kj::ArrayPtr<kj::byte> output,
      uint32_t outputLength);
  void setInputBuffer(kj::ArrayPtr<const kj::byte> input);
  void setOutputBuffer(kj::ArrayPtr<kj::byte> output);
  void setFlush(int flush);
  kj::uint getAvailOut() const;
  void getAfterWriteResult(uint32_t* availIn, uint32_t* availOut) const;
  void setMode(ZlibMode _mode) {
    mode = _mode;
  }

  struct Options {
    jsg::Optional<int> flush;
    jsg::Optional<int> finishFlush;
    jsg::Optional<kj::uint> chunkSize;
    jsg::Optional<jsg::Dict<int>> params;
    jsg::Optional<kj::uint> maxOutputLength;
    JSG_STRUCT(flush, finishFlush, chunkSize, params, maxOutputLength);
  };

protected:
  ZlibMode mode;
  const uint8_t* nextIn = nullptr;
  uint8_t* nextOut = nullptr;
  size_t availIn = 0;
  size_t availOut = 0;
  BrotliEncoderOperation flush = BROTLI_OPERATION_PROCESS;

  // TODO(addaleax): These should not need to be stored here.
  // This is currently only done this way to make implementing ResetStream()
  // easier.
  brotli_alloc_func alloc_brotli = nullptr;
  brotli_free_func free_brotli = nullptr;
  void* alloc_opaque_brotli = nullptr;
};

class BrotliEncoderContext final: public BrotliContext {
public:
  static const ZlibMode Mode = ZlibMode::BROTLI_ENCODE;
  explicit BrotliEncoderContext(ZlibMode _mode);

  KJ_DISALLOW_COPY_AND_MOVE(BrotliEncoderContext);

  // Equivalent to Node.js' `DoThreadPoolWork` implementation.
  void work();
  kj::Maybe<CompressionError> initialize(
      brotli_alloc_func init_alloc_func, brotli_free_func init_free_func, void* init_opaque_func);
  kj::Maybe<CompressionError> resetStream();
  kj::Maybe<CompressionError> setParams(int key, uint32_t value);
  kj::Maybe<CompressionError> getError() const;

private:
  bool lastResult = false;
  kj::Own<BrotliEncoderStateStruct> state;
};

class BrotliDecoderContext final: public BrotliContext {
public:
  static const ZlibMode Mode = ZlibMode::BROTLI_DECODE;
  explicit BrotliDecoderContext(ZlibMode _mode);

  KJ_DISALLOW_COPY_AND_MOVE(BrotliDecoderContext);

  // Equivalent to Node.js' `DoThreadPoolWork` implementation.
  void work();
  kj::Maybe<CompressionError> initialize(
      brotli_alloc_func init_alloc_func, brotli_free_func init_free_func, void* init_opaque_func);
  kj::Maybe<CompressionError> resetStream();
  kj::Maybe<CompressionError> setParams(int key, uint32_t value);
  kj::Maybe<CompressionError> getError() const;

private:
  BrotliDecoderResult lastResult = BROTLI_DECODER_RESULT_SUCCESS;
  BrotliDecoderErrorCode error = BROTLI_DECODER_NO_ERROR;
  kj::String errorString;
  kj::Own<BrotliDecoderStateStruct> state;
};

// Implements utilities in support of the Node.js Zlib
class ZlibUtil final: public jsg::Object {
public:
  ZlibUtil() = default;
  ZlibUtil(jsg::Lock&, const jsg::Url&) {}

  // A custom allocator to be used by the zlib and brotli libraries
  // The current implementation stores allocations in a hash map.
  // TODO: Use an arena allocator implementation instead of hashing pointers in order to improve performance
  class Allocator final {
  public:
    static void* AllocForZlib(void* data, uInt items, uInt size);
    static void* AllocForBrotli(void* data, size_t size);
    static void FreeForZlib(void* data, void* pointer);

  private:
    kj::HashMap<void*, kj::Array<kj::byte>> allocations;
  };

  template <class CompressionContext>
  class CompressionStream: public jsg::Object {
  public:
    explicit CompressionStream(ZlibMode _mode): context_(_mode) {}
    CompressionStream() = default;
    // TODO(soon): Find a way to add noexcept(false) to this destructor.
    ~CompressionStream();
    KJ_DISALLOW_COPY_AND_MOVE(CompressionStream);

    static jsg::Ref<CompressionStream> constructor(ZlibModeValue mode);

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
    }

    void updateWriteResult();

    template <bool async>
    void write(jsg::Lock& js,
        int flush,
        jsg::Optional<kj::Array<kj::byte>> input,
        int inputOffset,
        int inputLength,
        kj::Array<kj::byte> output,
        int outputOffset,
        int outputLength);
    void reset(jsg::Lock& js);

    JSG_RESOURCE_TYPE(CompressionStream) {
      JSG_METHOD(close);
      JSG_METHOD_NAMED(write, template write<true>);
      JSG_METHOD_NAMED(writeSync, template write<false>);
      JSG_METHOD(reset);
      JSG_METHOD(setErrorHandler);
    }

  protected:
    CompressionContext* context() {
      return &context_;
    }

    void initializeStream(jsg::BufferSource _write_result, jsg::Function<void()> writeCallback);

    // Used to store allocations in Brotli* operations.
    // This declaration should be physically positioned before
    // context to avoid `heap-use-after-free` ASan error.
    Allocator allocator;

  private:
    CompressionContext context_;
    bool initialized = false;
    bool writing = false;
    bool pending_close = false;
    bool closed = false;

    // Equivalent to `write_js_callback` in Node.js
    jsg::Optional<jsg::Function<void()>> writeCallback;
    jsg::Optional<jsg::BufferSource> writeResult;
    jsg::Optional<CompressionStreamErrorHandler> errorHandler;
  };

  class ZlibStream final: public CompressionStream<ZlibContext> {
  public:
    explicit ZlibStream(ZlibMode mode): CompressionStream(mode) {}
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
    void params(jsg::Lock& js, int level, int strategy);

    JSG_RESOURCE_TYPE(ZlibStream) {
      JSG_INHERIT(CompressionStream<ZlibContext>);

      JSG_METHOD(initialize);
      JSG_METHOD(params);
    }
  };

  template <typename CompressionContext>
  class BrotliCompressionStream: public CompressionStream<CompressionContext> {
  public:
    explicit BrotliCompressionStream(ZlibMode _mode)
        : CompressionStream<CompressionContext>(_mode) {}
    KJ_DISALLOW_COPY_AND_MOVE(BrotliCompressionStream);
    static jsg::Ref<BrotliCompressionStream> constructor(ZlibModeValue mode);

    bool initialize(jsg::Lock& js,
        jsg::BufferSource params,
        jsg::BufferSource writeResult,
        jsg::Function<void()> writeCallback);

    void params() {
      // Currently a no-op, and not accessed from JS land.
      // At some point Brotli may support changing parameters on the fly,
      // in which case we can implement this and a JS equivalent similar to
      // the zlib Params() function.
    }

    JSG_RESOURCE_TYPE(BrotliCompressionStream) {
      JSG_INHERIT(CompressionStream<CompressionContext>);

      JSG_METHOD(initialize);
      JSG_METHOD(params);
    }

    CompressionContext* context() {
      return this->CompressionStream<CompressionContext>::context();
    }
  };

  using InputSource = kj::OneOf<jsg::NonCoercible<kj::String>, kj::Array<kj::byte>>;
  using CompressCallbackArg = kj::OneOf<jsg::JsValue, kj::Array<kj::byte>>;
  using CompressCallback = jsg::Function<void(CompressCallbackArg)>;

  uint32_t crc32Sync(InputSource data, uint32_t value);
  void zlibWithCallback(jsg::Lock& js,
      InputSource data,
      ZlibContext::Options options,
      ZlibModeValue mode,
      CompressCallback cb);
  kj::Array<kj::byte> zlibSync(InputSource data, ZlibContext::Options options, ZlibModeValue mode);

  template <typename Context>
  kj::Array<kj::byte> brotliSync(InputSource data, BrotliContext::Options options);
  template <typename Context>
  void brotliWithCallback(
      jsg::Lock& js, InputSource data, BrotliContext::Options options, CompressCallback cb);

  JSG_RESOURCE_TYPE(ZlibUtil) {
    JSG_METHOD_NAMED(crc32, crc32Sync);
    JSG_METHOD(zlibSync);
    JSG_METHOD_NAMED(zlib, zlibWithCallback);

    JSG_METHOD_NAMED(brotliDecompressSync, template brotliSync<BrotliDecoderContext>);
    JSG_METHOD_NAMED(brotliCompressSync, template brotliSync<BrotliEncoderContext>);
    JSG_METHOD_NAMED(brotliDecompress, template brotliWithCallback<BrotliDecoderContext>);
    JSG_METHOD_NAMED(brotliCompress, template brotliWithCallback<BrotliEncoderContext>);

    JSG_NESTED_TYPE(ZlibStream);
    JSG_NESTED_TYPE_NAMED(BrotliCompressionStream<BrotliEncoderContext>, BrotliEncoder);
    JSG_NESTED_TYPE_NAMED(BrotliCompressionStream<BrotliDecoderContext>, BrotliDecoder);

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
  }
};

#define EW_NODE_ZLIB_ISOLATE_TYPES                                                                 \
  api::node::ZlibUtil, api::node::ZlibUtil::ZlibStream,                                            \
      api::node::ZlibUtil::BrotliCompressionStream<api::node::BrotliEncoderContext>,               \
      api::node::ZlibUtil::BrotliCompressionStream<api::node::BrotliDecoderContext>,               \
      api::node::ZlibUtil::CompressionStream<api::node::ZlibContext>,                              \
      api::node::ZlibUtil::CompressionStream<api::node::BrotliEncoderContext>,                     \
      api::node::ZlibUtil::CompressionStream<api::node::BrotliDecoderContext>,                     \
      api::node::ZlibContext::Options, api::node::BrotliContext::Options

}  // namespace workerd::api::node

KJ_DECLARE_NON_POLYMORPHIC(BrotliEncoderStateStruct)
KJ_DECLARE_NON_POLYMORPHIC(BrotliDecoderStateStruct)
