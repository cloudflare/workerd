// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-99.
// When a BYOB read's resizable ArrayBuffer is shrunk to zero via the
// byobRequest.view.buffer alias, close() → handleMaybeClose() uses a
// stale cached byteLength to compute the destination ArrayPtr, writing
// into decommitted (PROT_NONE) pages → SIGSEGV.
// Post-fix: the close should complete without crashing.
export const closeAfterResizableBufferShrunkToZero = {
  async test() {
    let ctrl;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
    });
    const reader = rs.getReader({ mode: 'byob' });

    // Resizable buffer large enough that resize(0) decommits pages.
    const rab = new ArrayBuffer(65536, { maxByteLength: 65536 });
    const view = new Uint8Array(rab);

    // BYOB read with min == full size so a small enqueue won't fulfill it.
    const readPromise = reader.read(view, { min: 65536 });

    await Promise.resolve();
    await Promise.resolve();

    // Enqueue a small chunk — buffered, read stays pending.
    ctrl.enqueue(new Uint8Array(100).fill(0x41));

    // Materialize byobRequest → exposes a new resizable ArrayBuffer
    // over the same BackingStore.
    const byobReq = ctrl.byobRequest;
    const byobBuf = byobReq.view.buffer;
    ok(byobBuf.resizable, 'buffer should be resizable');

    // Shrink the backing store to 0 bytes — decommits pages.
    byobBuf.resize(0);

    // close() → handleMaybeClose() should not SIGSEGV.
    // Pre-fix: crashes writing into PROT_NONE pages.
    // Post-fix: completes without crash.
    ctrl.close();

    // The read may resolve or reject depending on how the implementation
    // handles the resized buffer — either is acceptable. The important
    // thing is no SIGSEGV.
    await readPromise;
  },
};
