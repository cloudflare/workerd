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

#include <zlib.h>

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

  // The canonical name for a zlib return code (e.g. "Z_DATA_ERROR"); "Z_UNKNOWN_ERROR" for
  // unrecognized codes.
  static kj::StringPtr errorCodeName(int code);

  // Maps a Compression Streams spec format ("gzip" | "deflate" | "deflate-raw") to its
  // windowBits value; kj::none for anything else.
  static kj::Maybe<int> windowBitsForWebFormat(kj::StringPtr format);

 private:
  z_stream stream = {};
  Mode mode = Mode::COMPRESS;
  bool initialized = false;
  bool ended = false;
};

}  // namespace workerd::api
