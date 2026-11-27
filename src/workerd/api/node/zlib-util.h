// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.
#pragma once

#include <workerd/api/compression.h>
#include <workerd/jsg/jsg.h>

#include <zlib.h>
#include <zstd.h>

#include <kj/array.h>
#include <kj/one-of.h>
#include <kj/vector.h>

// The following implementation is adapted from Node.js
// and therefore follows Node.js style as opposed to kj style.
// Latest implementation of Node.js zlib can be found at:
// https://github.com/nodejs/node/blob/main/src/node_zlib.cc
namespace workerd::api::node {

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

// When possible, we intentionally override chunkSize to a value that is likely to perform better
static constexpr auto ZLIB_PERFORMANT_CHUNK_SIZE = 40 * 1024;

class ZlibContext final {
 public:
  explicit ZlibContext(CompressionAllocator& allocator, ZlibMode _mode)
      : core(allocator),
        mode(_mode) {}
  ~ZlibContext() noexcept(false);

  KJ_DISALLOW_COPY_AND_MOVE(ZlibContext);

  void setBuffers(kj::ArrayPtr<kj::byte> input, kj::ArrayPtr<kj::byte> output);

  void setInputBuffer(kj::ArrayPtr<const kj::byte> input);
  void setOutputBuffer(kj::ArrayPtr<kj::byte> output);

  // Clear all buffer pointers from z_stream to prevent stale pointer access.
  // Must be called after each write operation completes and results have been
  // captured, so that subsequent operations (e.g. deflateParams) cannot use
  // dangling pointers into freed backing stores.
  //
  // Note: zlib's deflate() rejects next_out == NULL with Z_STREAM_ERROR even
  // when avail_out == 0, so we point next_out at a valid dummy byte instead.
  // With avail_out == 0, no data will actually be written to it.
  void clearBuffers() {
    auto& stream = core.raw();
    stream.next_in = nullptr;
    stream.avail_in = 0;
    stream.next_out = &dummyByte;
    stream.avail_out = 0;
  }

  int getFlush() const {
    return flush;
  };
  void setFlush(int value) {
    flush = value;
  };
  // Function signature is same as Node.js implementation.
  // Ref: https://github.com/nodejs/node/blob/9edf4a0856681a7665bd9dcf2ca7cac252784b98/src/node_zlib.cc#L880
  void getAfterWriteResult(uint32_t* availIn, uint32_t* availOut) const {
    *availIn = core.availIn();
    *availOut = core.availOut();
  }
  void setMode(ZlibMode value) {
    mode = value;
  };
  kj::Maybe<CompressionError> resetStream();
  kj::Maybe<CompressionError> getError() const;

  // Equivalent to Node.js' `DoThreadPoolWork` function.
  // Ref: https://github.com/nodejs/node/blob/9edf4a0856681a7665bd9dcf2ca7cac252784b98/src/node_zlib.cc#L760
  void work();

  // Returns true when the zlib stream has reached Z_STREAM_END, indicating
  // that all compressed data has been fully processed.
  bool isStreamEnd() const {
    return err == Z_STREAM_END;
  }

  uint getAvailIn() const {
    return core.availIn();
  };
  void setAvailIn(uint value) {
    core.raw().avail_in = value;
  };
  uint getAvailOut() const {
    return core.availOut();
  }
  void setAvailOut(uint value) {
    core.raw().avail_out = value;
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

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("dictionary", dictionary.size());
  }

 private:
  bool initializeZlib();
  kj::Maybe<CompressionError> setDictionary();

  CompressionError constructError(kj::StringPtr message) const {
    auto streamMsg = core.msg();
    if (streamMsg != nullptr) message = kj::mv(streamMsg);

    return {kj::str(message), kj::str(ZlibStream::errorCodeName(err)), err};
  };

  // The shared z_stream core (api/compression.h) owns the stream structure and its
  // lifecycle; the Node-specific machinery -- mode bookkeeping (including the UNZIP
  // gzip-sniffing mode reassignment), dictionaries, deflateParams, and the Node-fidelity
  // error surface -- lives here and reaches the structure through core.raw() where no
  // core helper applies.
  ZlibStream core;
  ZlibMode mode = ZlibMode::NONE;
  int flush = Z_NO_FLUSH;
  int windowBits = 0;
  int level = 0;
  int memLevel = 0;
  int strategy = 0;
  kj::Vector<kj::byte> dictionary;

  int err = Z_OK;
  unsigned int gzip_id_bytes_read = 0;
  // Dummy byte target for clearBuffers(). zlib's deflate() rejects
  // next_out == NULL even when avail_out == 0, so we need a valid address.
  Bytef dummyByte = 0;
};

using CompressionStreamErrorHandler = jsg::Function<void(int, kj::StringPtr, kj::StringPtr)>;

// Implements utilities in support of the Node.js Zlib
class ZlibUtil final: public jsg::Object {
 public:
  ZlibUtil() = default;
  ZlibUtil(jsg::Lock&, const jsg::Url&) {}

  // Stateless module object, safe to recreate for a worker started from a snapshot.
  static constexpr bool jsgSnapshotStateless = true;

  template <class CompressionContext>
  class CompressionStream: public jsg::Object {
   public:
    explicit CompressionStream(
        ZlibMode _mode, kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
        : allocator(kj::mv(externalMemoryTarget)),
          context_(allocator, _mode) {}
    // TODO(soon): Find a way to add noexcept(false) to this destructor.
    ~CompressionStream();
    KJ_DISALLOW_COPY_AND_MOVE(CompressionStream);

    static jsg::Ref<CompressionStream> constructor(jsg::Lock& js, ZlibModeValue mode);

    void close();
    void emitError(jsg::Lock& js, const CompressionError& error);
    template <bool async>
    void writeStream(
        jsg::Lock& js, int flush, kj::ArrayPtr<kj::byte> input, kj::ArrayPtr<kj::byte> output);
    void setErrorHandler(CompressionStreamErrorHandler handler) {
      errorHandler = kj::mv(handler);
    }

    void updateWriteResult(jsg::Lock& js, uint32_t availIn, uint32_t availOut);

    template <bool async>
    void write(jsg::Lock& js,
        int flush,
        jsg::Optional<jsg::JsBufferSource> input,
        uint32_t inputOffset,
        uint32_t inputLength,
        jsg::JsBufferSource output,
        uint32_t outputOffset,
        uint32_t outputLength);
    void reset(jsg::Lock& js);

    JSG_RESOURCE_TYPE(CompressionStream) {
      JSG_METHOD(close);
      JSG_METHOD_NAMED(write, template write<true>);
      JSG_METHOD_NAMED(writeSync, template write<false>);
      JSG_METHOD(reset);
      JSG_METHOD(setErrorHandler);
    }

    // writeCallback and errorHandler typically capture `this`'s JS wrapper
    // (see internal_zlib_base.ts), forming a JS<->C++ cycle that V8 can only
    // collect with this tracing.
    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(writeCallback, writeResult, errorHandler);
    }

    void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
      allocator.visitForMemoryInfo(tracker);
      context_.visitForMemoryInfo(tracker);
      tracker.trackField("writeCallback", writeCallback);
      tracker.trackField("writeResult", writeResult);
      tracker.trackField("errorHandler", errorHandler);
    }

   protected:
    CompressionContext* context() {
      return &context_;
    }

    void initializeStream(
        jsg::Lock& js, jsg::JsArrayBufferView& _write_result, jsg::Function<void()> writeCallback);

    // Used to store allocations in Brotli* operations.
    // This declaration should be physically positioned before
    // context to avoid `heap-use-after-free` ASan error.
    CompressionAllocator allocator;

   private:
    CompressionContext context_;
    bool initialized = false;
    bool writing = false;
    bool pending_close = false;
    bool closed = false;

    // Equivalent to `write_js_callback` in Node.js
    jsg::Optional<jsg::Function<void()>> writeCallback;
    jsg::Optional<jsg::JsRef<jsg::JsArrayBufferView>> writeResult;
    jsg::Optional<CompressionStreamErrorHandler> errorHandler;
  };

  class ZlibStream final: public CompressionStream<ZlibContext> {
   public:
    explicit ZlibStream(
        ZlibMode mode, kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
        : CompressionStream(mode, kj::mv(externalMemoryTarget)) {}
    KJ_DISALLOW_COPY_AND_MOVE(ZlibStream);
    static jsg::Ref<ZlibStream> constructor(jsg::Lock& js, ZlibModeValue mode);

    // Instance methods
    void initialize(jsg::Lock& js,
        int windowBits,
        int level,
        int memLevel,
        int strategy,
        jsg::JsArrayBufferView writeState,
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
    explicit BrotliCompressionStream(
        ZlibMode _mode, kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
        : CompressionStream<CompressionContext>(_mode, kj::mv(externalMemoryTarget)) {}
    KJ_DISALLOW_COPY_AND_MOVE(BrotliCompressionStream);
    static jsg::Ref<BrotliCompressionStream> constructor(jsg::Lock& js, ZlibModeValue mode);

    bool initialize(jsg::Lock& js,
        jsg::JsArrayBufferView params,
        jsg::JsArrayBufferView writeResult,
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

  template <typename CompressionContext>
  class ZstdCompressionStream: public CompressionStream<CompressionContext> {
   public:
    explicit ZstdCompressionStream(
        ZlibMode _mode, kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget)
        : CompressionStream<CompressionContext>(_mode, kj::mv(externalMemoryTarget)) {}
    KJ_DISALLOW_COPY_AND_MOVE(ZstdCompressionStream);
    static jsg::Ref<ZstdCompressionStream> constructor(jsg::Lock& js, ZlibModeValue mode);

    bool initialize(jsg::Lock& js,
        jsg::JsArrayBufferView params,
        jsg::JsArrayBufferView writeResult,
        jsg::Function<void()> writeCallback,
        jsg::Optional<uint64_t> pledgedSrcSize);

    void params() {
      // Currently a no-op, and not accessed from JS land.
    }

    JSG_RESOURCE_TYPE(ZstdCompressionStream) {
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

  // Returns a new zlib.constants object. The constants are not static constants on ZlibUtil
  // because each isolate would then build and instantiate a template property for each one.
  jsg::JsObject getConstants(jsg::Lock& js);
  void zlibWithCallback(jsg::Lock& js,
      InputSource data,
      ZlibContext::Options options,
      ZlibModeValue mode,
      CompressCallback cb);
  kj::Array<kj::byte> zlibSync(
      jsg::Lock& js, InputSource data, ZlibContext::Options options, ZlibModeValue mode);

  template <typename Context>
  kj::Array<kj::byte> brotliSync(jsg::Lock& js, InputSource data, BrotliContext::Options options);
  template <typename Context>
  void brotliWithCallback(
      jsg::Lock& js, InputSource data, BrotliContext::Options options, CompressCallback cb);

  template <typename Context>
  kj::Array<kj::byte> zstdSync(jsg::Lock& js, InputSource data, ZstdContext::Options options);
  template <typename Context>
  void zstdWithCallback(
      jsg::Lock& js, InputSource data, ZstdContext::Options options, CompressCallback cb);

  JSG_RESOURCE_TYPE(ZlibUtil) {
    JSG_METHOD_NAMED(crc32, crc32Sync);
    JSG_METHOD(zlibSync);
    JSG_METHOD_NAMED(zlib, zlibWithCallback);

    JSG_METHOD_NAMED(brotliDecompressSync, template brotliSync<BrotliDecoderContext>);
    JSG_METHOD_NAMED(brotliCompressSync, template brotliSync<BrotliEncoderContext>);
    JSG_METHOD_NAMED(brotliDecompress, template brotliWithCallback<BrotliDecoderContext>);
    JSG_METHOD_NAMED(brotliCompress, template brotliWithCallback<BrotliEncoderContext>);

    JSG_METHOD_NAMED(zstdDecompressSync, template zstdSync<ZstdDecoderContext>);
    JSG_METHOD_NAMED(zstdCompressSync, template zstdSync<ZstdEncoderContext>);
    JSG_METHOD_NAMED(zstdDecompress, template zstdWithCallback<ZstdDecoderContext>);
    JSG_METHOD_NAMED(zstdCompress, template zstdWithCallback<ZstdEncoderContext>);

    JSG_NESTED_TYPE(ZlibStream);
    JSG_NESTED_TYPE_NAMED(BrotliCompressionStream<BrotliEncoderContext>, BrotliEncoder);
    JSG_NESTED_TYPE_NAMED(BrotliCompressionStream<BrotliDecoderContext>, BrotliDecoder);
    JSG_NESTED_TYPE_NAMED(ZstdCompressionStream<ZstdEncoderContext>, ZstdEncoder);
    JSG_NESTED_TYPE_NAMED(ZstdCompressionStream<ZstdDecoderContext>, ZstdDecoder);

    JSG_METHOD(getConstants);
  }
};

#define EW_NODE_ZLIB_ISOLATE_TYPES                                                                 \
  api::node::ZlibUtil, api::node::ZlibUtil::ZlibStream,                                            \
      api::node::ZlibUtil::BrotliCompressionStream<api::BrotliEncoderContext>,                     \
      api::node::ZlibUtil::BrotliCompressionStream<api::BrotliDecoderContext>,                     \
      api::node::ZlibUtil::ZstdCompressionStream<api::ZstdEncoderContext>,                         \
      api::node::ZlibUtil::ZstdCompressionStream<api::ZstdDecoderContext>,                         \
      api::node::ZlibUtil::CompressionStream<api::node::ZlibContext>,                              \
      api::node::ZlibUtil::CompressionStream<api::BrotliEncoderContext>,                           \
      api::node::ZlibUtil::CompressionStream<api::BrotliDecoderContext>,                           \
      api::node::ZlibUtil::CompressionStream<api::ZstdEncoderContext>,                             \
      api::node::ZlibUtil::CompressionStream<api::ZstdDecoderContext>,                             \
      api::node::ZlibContext::Options, api::BrotliContext::Options, api::ZstdContext::Options

}  // namespace workerd::api::node
