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

}  // namespace workerd::api
