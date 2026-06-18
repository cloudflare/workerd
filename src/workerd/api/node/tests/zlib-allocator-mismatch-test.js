// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-356:
// Process abort via allocator mismatch in node:zlib ZlibStream when
// params()/reset()/write() is called before initialize().
//
// The bug: ZlibStream::initialize() was the only place that configures
// the z_stream to use CompressionAllocator, but params()/reset() could
// call initializeZlib() first, causing zlib to use its default malloc
// allocator. When initialize() later overwrote stream.zfree to FreeForZlib,
// the malloc'd allocations were not known by the allocator, resulting in a
// fatal JSG_REQUIRE failure in FreeForZlib during GC finalization.

import zlib from 'node:zlib';

// Calling params() before initialize() must not crash the process.
export const paramsBeforeInitializeTest = {
  test() {
    const ZlibStream = zlib.createDeflate()._handle.constructor;

    (() => {
      const h = new ZlibStream(2 /* INFLATE */);
      // Call params() BEFORE initialize(). Pre-fix, this triggers initializeZlib() ->
      // inflateInit2() with stream.zalloc==NULL, causing zlib to use its default malloc allocator.
      h.params(6, 0);
      // Now call initialize(). Pre-fix, this overwrites stream.zfree to FreeForZlib without the
      // malloc'd allocations being tracked.
      h.initialize(15, 6, 8, 0, new Uint32Array(2), () => {});
    })();

    // The stream should be destroyed without aborting the process. The test harness will also check
    // this.
    gc();
  },
};

// Calling reset() before initialize() must not crash the process.
export const resetBeforeInitializeTest = {
  test() {
    const ZlibStream = zlib.createDeflate()._handle.constructor;

    (() => {
      const h = new ZlibStream(2 /* INFLATE */);
      // reset() also calls initializeZlib() internally.
      h.reset();
      h.initialize(15, 6, 8, 0, new Uint32Array(2), () => {});
    })();

    // The stream should be destroyed without aborting the process. The test harness will also check
    // this.
    gc();
  },
};
