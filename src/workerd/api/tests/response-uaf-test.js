// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for a use-after-free: creating a Response from an ArrayBuffer,
// then transferring the buffer via structuredClone and collecting the clone,
// must not cause the Response body read to access freed memory.
//
// Without the fix, this crashes under ASAN with:
//   heap-use-after-free READ of size 1024

export default {
  async test() {
    // Fixed-size ArrayBuffer (non-resizable). The UAF is not specific to
    // resizable buffers — it affects any ArrayBuffer whose backing store can
    // be transferred away.
    const buffer = new ArrayBuffer(1024);
    new Uint8Array(buffer).fill(0x42);

    const res = new Response(buffer);

    // Transfer detaches the original buffer. The clone becomes the sole
    // JS-visible owner of the backing store.
    structuredClone(buffer, { transfer: [buffer] });

    // Collect the clone (which was not assigned to a variable). This frees
    // the backing store memory if nothing else prevents it.
    gc();

    // Reading the body must not touch freed memory.
    const result = new Uint8Array(await res.arrayBuffer());
    if (result.length !== 1024) {
      throw new Error(`Expected 1024 bytes, got ${result.length}`);
    }
    // Verify the data is intact (not garbage from freed memory).
    for (let i = 0; i < result.length; i++) {
      if (result[i] !== 0x42) {
        throw new Error(
          `Byte ${i}: expected 0x42, got 0x${result[i].toString(16)}`
        );
      }
    }
  },
};
