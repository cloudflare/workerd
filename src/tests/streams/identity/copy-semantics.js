// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writes copy their bytes at write() time; they never alias or transfer the
// caller's buffer.

import { ok, strictEqual } from 'node:assert';

export const writeCopiesData = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const original = new Uint8Array([1, 2, 3]);
    const writePromise = writer.write(original);
    const { value } = await reader.read();
    // Mutating the original after the write must not affect the read value.
    original[0] = 99;
    strictEqual(value[0], 1);
    // The delivered chunk must not alias the caller's buffer.
    ok(value.buffer !== original.buffer);
    // The caller's buffer must not have been detached by the write.
    strictEqual(original.byteLength, 3);
    await writePromise;
    await writer.close();
  },
};
