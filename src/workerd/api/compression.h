// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Shared compression machinery. This is the common home for the compression primitives used
// by the three JavaScript-facing compression surfaces:
//
//   - the web CompressionStream/DecompressionStream pair (api/streams/compression.{h,c++}),
//   - the node:zlib module (api/node/zlib-util.{h,c++}),
//   - the TypeScript streams implementation's pair (src/per_isolate/webstreams/), which
//     drives the synchronous codec through a handle minted by the C++ side.
//
// The consumers keep their own semantics (spec-pinned TypeErrors vs. Node-fidelity error
// codes vs. TS orchestration); what lives here is mechanism. The fetch content-encoding path
// (system-streams.c++) wraps kj's async gzip/brotli streams directly and does not use this.

#include <workerd/jsg/jsg.h>

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

namespace workerd::api {

// A custom allocator to be used by the zlib and brotli libraries.
// The allocator should not and can not safely hold a reference to the jsg::Lock
// instance. Therefore, we lookup the current jsg::Lock instance from the
// isolate pointer and use that to get the external memory adjustment.
class CompressionAllocator final {
 public:
  CompressionAllocator(kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget);

  static void* AllocForZlib(void* data, uInt items, uInt size);
  static void* AllocForBrotli(void* data, size_t size);
  static void FreeForZlib(void* data, void* pointer);

 private:
  struct Allocation {
    kj::Array<kj::byte> data;
    kj::Maybe<jsg::ExternalMemoryAdjustment> memoryAdjustment = kj::none;
  };

  kj::Arc<const jsg::ExternalMemoryTarget> externalMemoryTarget;
  kj::HashMap<void*, Allocation> allocations;
};

// The shared z_stream wrapper: owns the stream structure and its init/reset/end lifecycle,
// the input/output buffer plumbing, and the raw deflate()/inflate() step. Mechanism only --
// consumers interpret the returned zlib codes according to their own policies (the web pair
// translates to its spec-pinned TypeErrors; node:zlib to Node-fidelity CompressionError
// codes), and node-specific zlib features (dictionaries, deflateParams) reach the structure
// through raw() until they grow shared consumers.
// Backend table over the zlib C API. Two implementations back it: native
// (chromium) zlib and zlib-rs (memory-safe Rust, see zlib-rs-bridge.h). The
// backend is chosen per stream by the compression-rs autogate at construction.
// The z_stream ABI is identical between the two.
struct ZlibBackend {
  int (*initDeflate)(z_stream* strm, int level, int windowBits, int memLevel, int strategy);
  int (*initInflate)(z_stream* strm, int windowBits);
  int (*runDeflate)(z_stream* strm, int flush);
  int (*runInflate)(z_stream* strm, int flush);
  int (*endDeflate)(z_stream* strm);
  int (*endInflate)(z_stream* strm);
  int (*resetDeflate)(z_stream* strm);
  int (*resetInflate)(z_stream* strm);
  int (*setDeflateParams)(z_stream* strm, int level, int strategy);
  int (*setDeflateDictionary)(z_stream* strm, const kj::byte* dictionary, uint32_t dictLength);
  int (*setInflateDictionary)(z_stream* strm, const kj::byte* dictionary, uint32_t dictLength);
};

// Selects the backing zlib implementation via the compression-rs autogate.
const ZlibBackend& selectZlibBackend();

class ZlibStream final {
 public:
  enum class Mode { COMPRESS, DECOMPRESS };

  struct Options {
    // Final windowBits, already including any format adjustment (e.g. +16 for gzip,
    // negated for raw); see windowBitsForWebFormat() and the node mode adjustments.
    int windowBits;
    // The remaining options apply to COMPRESS only.
    int level = Z_DEFAULT_COMPRESSION;
    int memLevel = 8;
    int strategy = Z_DEFAULT_STRATEGY;
  };

  // The allocator must outlive this object. Construction only wires the allocation hooks;
  // the underlying stream is not initialized until init() (node initializes lazily).
  explicit ZlibStream(CompressionAllocator& allocator);
  KJ_DISALLOW_COPY_AND_MOVE(ZlibStream);

  // Ends the stream if initialized, ignoring the result; consumers that need an
  // error-checked shutdown (node) call end() explicitly first.
  ~ZlibStream() noexcept(false);

  // deflateInit2/inflateInit2. Returns the zlib error code on failure, kj::none on success.
  // May be called at most once.
  kj::Maybe<int> init(Mode mode, Options options);

  // deflateReset/inflateReset. Precondition: initialized.
  kj::Maybe<int> reset();

  // deflateEnd/inflateEnd. Returns the zlib code (Z_OK if never initialized or already
  // ended). Idempotent.
  int end();

  // One deflate()/inflate() step with the given flush mode, over the buffers established by
  // setInput()/setOutput() (tracked in the stream's next/avail fields across calls).
  // Precondition: initialized.
  int run(int flush);

  void setInput(kj::ArrayPtr<const kj::byte> input);
  void setOutput(kj::ArrayPtr<kj::byte> output);
  size_t availIn() const;
  size_t availOut() const;

  // The stream's current error message (stream.msg), or empty if none.
  kj::StringPtr msg() const;

  bool isInitialized() const {
    return initialized;
  }
  Mode getMode() const {
    return mode;
  }

  // Escape hatch for consumer-specific zlib calls that take the z_stream directly
  // (deflateSetDictionary, inflateSetDictionary, deflateParams, ...). Precondition:
  // initialized (except for consumers wiring additional fields pre-init).
  z_stream& raw() {
    return stream;
  }

  // The zlib implementation backing this stream, for consumer-specific calls
  // made through raw().
  const ZlibBackend& getZlibBackend() const {
    return backend;
  }

  // The canonical name for a zlib return code (e.g. "Z_DATA_ERROR"); "Z_UNKNOWN_ERROR" for
  // unrecognized codes.
  static kj::StringPtr errorCodeName(int code);

  // Maps a Compression Streams spec format ("gzip" | "deflate" | "deflate-raw") to its
  // windowBits value; kj::none for anything else.
  static kj::Maybe<int> windowBitsForWebFormat(kj::StringPtr format);

 private:
  const ZlibBackend& backend = selectZlibBackend();
  z_stream stream = {};
  Mode mode = Mode::COMPRESS;
  bool initialized = false;
  bool ended = false;
};

// The synchronous codec stage behind the web Compression Streams pairs (both the legacy C++
// frontend in api/streams/compression.c++ and the TypeScript streams implementation's
// frontend): the Compression Streams spec policy — spec-pinned TypeErrors and the
// strict_compression_checks handling — over the shared ZlibStream core, plus the stage's own
// output buffer. Frontends are orchestration shells around one of these; ALL asynchrony is
// frontend-owned.
//
// PACING IS DELIBERATELY EAGER: push() runs the codec over the whole input chunk
// synchronously, accumulating ALL produced output in the stage buffer. The spec runs the
// codec inside the TransformStream transform()/flush() algorithms, so corrupt input MUST
// reject the write (and a strict-mode incomplete stream MUST reject the close) — that error
// timing is observable and WPT-pinned. Demand-paced production (pump only what a reader
// asked for) is therefore NOT valid on any frontend where write settlement is observable; it
// remains a possible future policy for fused native pipelines that own both ends.
class CodecStage final {
 public:
  using Mode = ZlibStream::Mode;

  enum class Flags {
    NONE,
    // The strict_compression_checks compat flag's decompression checks (trailing data after
    // end of stream; close with incomplete data).
    STRICT,
  };

  // `format` must be a valid web format ("gzip" | "deflate" | "deflate-raw"); the
  // JS-visible format validation (with its spec-pinned TypeError) belongs to the frontends.
  explicit CodecStage(Mode mode,
      kj::StringPtr format,
      Flags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget);
  KJ_DISALLOW_COPY_AND_MOVE(CodecStage);

  // Runs the codec over one input chunk to exhaustion, synchronously. The input is fully
  // consumed before this returns (zlib copies what it needs into its own window), so the
  // caller's buffer is not retained. Produced output accumulates in the stage buffer.
  // Throws on codec error — the caller owns its own state/teardown response.
  void push(kj::ArrayPtr<const kj::byte> input);

  // Finishes the codec (Z_FINISH), which also runs the strict-mode end checks for
  // decompression (trailing data / incomplete stream). Idempotent: repeat calls are no-ops,
  // preserving the historical allowance for multiple end() calls.
  void end();

  // Copies up to dest.size() buffered output bytes into dest, returning the count copied.
  size_t pull(kj::ArrayPtr<kj::byte> dest);

  size_t available();
  bool empty();

  // Teardown (frontend cancel/abort path): drops all buffered output.
  void clear();

 private:
  // The per-pump policy layer: one deflate()/inflate() step into the scratch buffer, with
  // the spec's TypeErrors applied to the result. The strict-mode checks are a separate step
  // (enforceStrictChecks) so the stage can buffer an erroring iteration's output BEFORE the
  // strict error throws — the final valid bytes are still deliverable, per the WPT-pinned
  // output-then-error order.
  class Context {
   public:
    struct Result {
      bool success = false;
      int result = Z_OK;
      kj::ArrayPtr<const kj::byte> buffer;
    };

    explicit Context(Mode mode,
        kj::StringPtr format,
        Flags flags,
        kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget);
    KJ_DISALLOW_COPY_AND_MOVE(Context);

    void setInput(const void* in, size_t size);
    Result pumpOnce(int flush);
    void enforceStrictChecks(int flush, const Result& result);

   private:
    CompressionAllocator allocator;
    ZlibStream stream;
    kj::byte buffer[16384];

    // For the eponymous compatibility flag
    Flags strictCompression;
  };

  // Buffer class based on kj::Vector that erases data that has been read from it lazily to
  // avoid excessive copying when reading a larger amount of buffered data in small chunks.
  // validSize is used to track the amount of data that has not been read back yet.
  class LazyBuffer {
   public:
    // Return a chunk of data and mark it as invalid. The returned chunk remains valid until
    // data is shifted, cleared or destructor is called. maybeShift() should be called after
    // the returned data has been processed.
    kj::ArrayPtr<kj::byte> take(size_t readSize);

    // Shift the output only if doing so results in reducing vector size by at least 1 KiB
    // and 1/8 of its size to avoid copying for small reads.
    void maybeShift();

    void write(kj::ArrayPtr<const kj::byte> chunk);
    void clear();

    // The size of the valid data that has not been read back yet. This may be smaller than
    // the size of the internal vector, which is not relevant to consumers.
    size_t size();
    bool empty();

   private:
    kj::Vector<kj::byte> output;
    size_t validSize = 0;
  };

  void pump(int flush);

  Context context;
  LazyBuffer output;
  bool finished = false;
};

// The synchronous codec handle for the TypeScript streams implementation's
// CompressionStream/DecompressionStream pair (webstreams/compression.ts): a thin internal
// JSG resource over CodecStage. Minted exclusively by the bootstrap's
// utils.newCompressionCodec() (see newCompressionCodecCallback below and
// per-isolate-bootstrap.c++); never registered as a global or nested type, so instances are
// reachable only by the bootstrap module that created them — user code cannot obtain one,
// and the type is kept out of the generated TypeScript types.
//
// All methods are synchronous and IoContext-free: compression is pure CPU, so the
// TypeScript pair's waiting is entirely V8 promise machinery, and global-scope construction
// is legal. External-memory accounting rides the stage's CompressionAllocator.
class CompressionCodec final: public jsg::Object {
 public:
  CompressionCodec(CodecStage::Mode mode,
      kj::StringPtr format,
      CodecStage::Flags flags,
      kj::Arc<const jsg::ExternalMemoryTarget>&& externalMemoryTarget);

  // Runs the codec over the chunk to exhaustion, synchronously and eagerly; the chunk is
  // fully consumed and not retained. Throws on codec error (rejecting the TS pair's write —
  // the spec's transform-time error timing).
  void push(jsg::JsBufferSource chunk);

  // Z_FINISH plus the strict-mode end checks; throws reject the TS pair's close. Idempotent.
  void end();

  // Copies up to view.size() buffered output bytes into the view, returning the count.
  uint32_t pullInto(jsg::JsBufferSource view);

  // double rather than uint32: a decompression stage buffer can in principle exceed uint32
  // range (the legacy implementation had the same unbounded buffering); JS numbers carry
  // the full size exactly.
  double available();

  JSG_RESOURCE_TYPE(CompressionCodec) {
    JSG_METHOD(push);
    JSG_METHOD(end);
    JSG_METHOD(pullInto);
    JSG_METHOD(available);

    // Internal plumbing type: keep it out of the generated TypeScript types.
    JSG_TS_OVERRIDE(type CompressionCodec = never);
  }

 private:
  CodecStage stage;
};

// The raw v8 callback behind the bootstrap's utils.newCompressionCodec(mode, format):
// validates the mode and format (with the spec TypeErrors the legacy constructors use),
// applies the strict_compression_checks compat flag for decompression, and allocates a
// CompressionCodec. Defined here (rather than in per-isolate-bootstrap.c++) so the
// compression knowledge stays with the machinery; the bootstrap only wires the utils member
// name to this callback.
void newCompressionCodecCallback(const v8::FunctionCallbackInfo<v8::Value>& info);

// =======================================================================================
// Codec mode plumbing and the brotli/zstd context families.
//
// These follow Node.js' structure (node:zlib is their consumer today) but live here
// because they are compression machinery, not Node bindings: CompressionStream/
// DecompressionStream are anticipated to grow brotli and zstd format support, at which
// point they gain web frontends over the same contexts.

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
  BROTLI_ENCODE,
  ZSTD_ENCODE,
  ZSTD_DECODE
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

class BrotliContext {
 public:
  explicit BrotliContext(CompressionAllocator& allocator, ZlibMode _mode)
      : allocator(allocator),
        mode(_mode) {}
  KJ_DISALLOW_COPY(BrotliContext);
  void setBuffers(kj::ArrayPtr<kj::byte> input, kj::ArrayPtr<kj::byte> output);
  void setInputBuffer(kj::ArrayPtr<const kj::byte> input);
  void setOutputBuffer(kj::ArrayPtr<kj::byte> output);
  void setFlush(int flush);
  kj::uint getAvailOut() const;
  void getAfterWriteResult(uint32_t* availIn, uint32_t* availOut) const;
  void setMode(ZlibMode _mode) {
    mode = _mode;
  }

  void clearBuffers() {
    nextIn = nullptr;
    nextOut = nullptr;
    availIn = 0;
    availOut = 0;
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
  CompressionAllocator& allocator;
  ZlibMode mode;
  const uint8_t* nextIn = nullptr;
  uint8_t* nextOut = nullptr;
  size_t availIn = 0;
  size_t availOut = 0;
  BrotliEncoderOperation flush = BROTLI_OPERATION_PROCESS;
};

class BrotliEncoderContext final: public BrotliContext {
 public:
  static const ZlibMode Mode = ZlibMode::BROTLI_ENCODE;
  explicit BrotliEncoderContext(CompressionAllocator& allocator, ZlibMode _mode);

  KJ_DISALLOW_COPY_AND_MOVE(BrotliEncoderContext);

  // Equivalent to Node.js' `DoThreadPoolWork` implementation.
  void work();
  kj::Maybe<CompressionError> initialize();
  kj::Maybe<CompressionError> resetStream();
  kj::Maybe<CompressionError> setParams(int key, uint32_t value);
  kj::Maybe<CompressionError> getError() const;
  bool isStreamEnd() const;

 private:
  bool lastResult = false;
  bool streamEnd = false;
  kj::Own<BrotliEncoderStateStruct> state;
};

class BrotliDecoderContext final: public BrotliContext {
 public:
  static const ZlibMode Mode = ZlibMode::BROTLI_DECODE;
  explicit BrotliDecoderContext(CompressionAllocator& allocator, ZlibMode _mode);

  KJ_DISALLOW_COPY_AND_MOVE(BrotliDecoderContext);

  // Equivalent to Node.js' `DoThreadPoolWork` implementation.
  void work();
  kj::Maybe<CompressionError> initialize();
  kj::Maybe<CompressionError> resetStream();
  kj::Maybe<CompressionError> setParams(int key, uint32_t value);
  kj::Maybe<CompressionError> getError() const;
  bool isStreamEnd() const;

 private:
  BrotliDecoderResult lastResult = BROTLI_DECODER_RESULT_SUCCESS;
  BrotliDecoderErrorCode error = BROTLI_DECODER_NO_ERROR;
  kj::String errorString;
  kj::Own<BrotliDecoderStateStruct> state;
};

class ZstdContext {
 public:
  explicit ZstdContext(ZlibMode _mode): mode(_mode) {}
  KJ_DISALLOW_COPY(ZstdContext);

  void setBuffers(kj::ArrayPtr<kj::byte> input, kj::ArrayPtr<kj::byte> output);
  void setInputBuffer(kj::ArrayPtr<const kj::byte> input);
  void setOutputBuffer(kj::ArrayPtr<kj::byte> output);
  void setFlush(int flush);
  kj::uint getAvailOut() const;
  void getAfterWriteResult(uint32_t* availIn, uint32_t* availOut) const;
  void setMode(ZlibMode _mode) {
    mode = _mode;
  }

  void clearBuffers() {
    input_ = {nullptr, 0, 0};
    output_ = {nullptr, 0, 0};
  }

  struct Options {
    jsg::Optional<int> flush;
    jsg::Optional<int> finishFlush;
    jsg::Optional<kj::uint> chunkSize;
    jsg::Optional<jsg::Dict<int>> params;
    jsg::Optional<kj::uint> maxOutputLength;
    jsg::Optional<uint64_t> pledgedSrcSize;
    JSG_STRUCT(flush, finishFlush, chunkSize, params, maxOutputLength, pledgedSrcSize);
  };

 protected:
  ZlibMode mode;
  ZSTD_inBuffer input_{nullptr, 0, 0};
  ZSTD_outBuffer output_{nullptr, 0, 0};
  ZSTD_EndDirective flush_ = ZSTD_e_continue;
};

class ZstdEncoderContext final: public ZstdContext {
 public:
  static const ZlibMode Mode = ZlibMode::ZSTD_ENCODE;
  explicit ZstdEncoderContext(ZlibMode _mode);
  explicit ZstdEncoderContext(CompressionAllocator& _allocator, ZlibMode _mode)
      : ZstdEncoderContext(_mode) {}
  KJ_DISALLOW_COPY_AND_MOVE(ZstdEncoderContext);

  void work();
  kj::Maybe<CompressionError> initialize(uint64_t pledgedSrcSize);
  kj::Maybe<CompressionError> resetStream();
  kj::Maybe<CompressionError> setParams(int key, int value);
  kj::Maybe<CompressionError> getError() const;
  bool isStreamEnd() const;

 private:
  size_t lastResult = 0;
  kj::Own<ZSTD_CCtx> cctx_;
  ZSTD_ErrorCode error_ = ZSTD_error_no_error;
};

class ZstdDecoderContext final: public ZstdContext {
 public:
  static const ZlibMode Mode = ZlibMode::ZSTD_DECODE;
  explicit ZstdDecoderContext(ZlibMode _mode);
  explicit ZstdDecoderContext(CompressionAllocator& _allocator, ZlibMode _mode)
      : ZstdDecoderContext(_mode) {}
  KJ_DISALLOW_COPY_AND_MOVE(ZstdDecoderContext);

  void work();
  kj::Maybe<CompressionError> initialize();
  kj::Maybe<CompressionError> resetStream();
  kj::Maybe<CompressionError> setParams(int key, int value);
  kj::Maybe<CompressionError> getError() const;
  bool isStreamEnd() const;

 private:
  size_t lastResult = 0;
  kj::Own<ZSTD_DCtx> dctx_;
  ZSTD_ErrorCode error_ = ZSTD_error_no_error;
  bool frameInProgress_ = false;
};

}  // namespace workerd::api

KJ_DECLARE_NON_POLYMORPHIC(BrotliEncoderStateStruct)
KJ_DECLARE_NON_POLYMORPHIC(BrotliDecoderStateStruct)
KJ_DECLARE_NON_POLYMORPHIC(ZSTD_CCtx)
KJ_DECLARE_NON_POLYMORPHIC(ZSTD_DCtx)
