// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Zero-length writes are no-ops: they resolve without delivering a chunk and
// without closing the stream.

import { strictEqual, deepStrictEqual } from 'node:assert';

export const zeroLengthUint8ArrayIsNoop = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    // A zero-length write resolves immediately, with no read pending.
    await writer.write(new Uint8Array(0));
    // Only the subsequent real data appears on the readable side.
    const writePromise = writer.write(new Uint8Array([42]));
    const { value, done } = await reader.read();
    strictEqual(done, false);
    deepStrictEqual([...value], [42]);
    await writePromise;
    await writer.close();
  },
};

export const zeroLengthArrayBufferIsNoop = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    await writer.write(new ArrayBuffer(0));
    const writePromise = writer.write(new Uint8Array([99]));
    const { value, done } = await reader.read();
    strictEqual(done, false);
    deepStrictEqual([...value], [99]);
    await writePromise;
    await writer.close();
  },
};

export const zeroLengthStringIsNoop = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    await writer.write('');
    const writePromise = writer.write('x');
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'x');
    await writePromise;
    await writer.close();
  },
};
