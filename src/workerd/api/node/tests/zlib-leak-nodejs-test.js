// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';
import {
  inflateSync,
  deflateSync,
  brotliCompressSync,
  brotliDecompressSync,
  createInflate,
} from 'node:zlib';

// Regression test for a memory leak that affected the slow path of the sync
// zlib convenience methods (i.e. `{ info: true }`). Each call constructed a
// JSG-bound CompressionStream wrapper that held a `jsg::Function` writeCallback
// capturing the JS handle, forming an uncollectable JS<->C++ cycle. The fix
// adds visitForGc() to CompressionStream so V8 can trace through the C++->JS
// edge and collect the cycle.
//
// We verify the fix by holding WeakRefs to the engines returned by `info: true`
// and asserting they are reclaimed after a GC. Without visitForGc tracing the
// cycle is immortal and the WeakRefs would still resolve.

const COMPRESSED_DEFLATE = deflateSync(new Uint8Array(1024));

async function awaitGc() {
  // Multiple GC passes with yields between them; gives the cycle collector
  // room to reclaim and avoids the conservative stack scanner pinning the
  // most recent allocation. scheduler.wait is a Workers-platform extension.
  for (let i = 0; i < 4; i++) {
    await scheduler.wait(0);
    globalThis.gc();
  }
}

// Performing the allocation loop inside a separate function ensures the
// caller's stack frame doesn't keep the last allocated engine rooted
// (V8's conservative stack scanner can otherwise pin the most recent
// value via a register/spill slot).
function collectRefs(fn) {
  const refs = [];
  for (let i = 0; i < 256; i++) {
    const r = fn();
    ok(r.engine, 'engine should be present on info result');
    refs.push(new WeakRef(r.engine));
  }
  return refs;
}

async function expectAllCollected(refs, label) {
  await awaitGc();
  let alive = 0;
  for (const ref of refs) {
    if (ref.deref() !== undefined) alive++;
  }
  // Allow at most a single straggler — V8's conservative stack scanner
  // can keep the most recently allocated object rooted via a stale
  // register/spill slot for one extra cycle. The leak we are testing
  // for is uncollectable cycles, which would leave all of them alive.
  ok(
    alive <= 1,
    `expected ${label} engines to be collected, ${alive} of ${refs.length} still alive`
  );
}

export const inflateSyncInfoCollects = {
  async test() {
    const refs = collectRefs(() =>
      inflateSync(COMPRESSED_DEFLATE, { info: true })
    );
    await expectAllCollected(refs, 'inflate');
  },
};

export const deflateSyncInfoCollects = {
  async test() {
    const input = new Uint8Array(1024);
    const refs = collectRefs(() => deflateSync(input, { info: true }));
    await expectAllCollected(refs, 'deflate');
  },
};

export const brotliSyncInfoCollects = {
  async test() {
    const input = new Uint8Array(1024);
    const compressed = brotliCompressSync(input);
    const refs = collectRefs(() =>
      brotliDecompressSync(compressed, { info: true })
    );
    await expectAllCollected(refs, 'brotli');
  },
};

// Specifically exercises the visitForGc path: createInflate() attaches both
// writeCallback and errorHandler, forming the JS<->C++ cycle. Dropping the
// reference without end()/destroy()/close() bypasses the eager-clear in
// close() and leaves only the GC visitor to break the cycle.
export const createInflateAbandonedCollects = {
  async test() {
    const refs = collectRefs(() => ({ engine: createInflate() }));
    await expectAllCollected(refs, 'createInflate-abandoned');
  },
};
