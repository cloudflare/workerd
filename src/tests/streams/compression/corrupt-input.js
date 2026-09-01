// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Corrupt input to DecompressionStream. The codec push runs eagerly inside
// write(), so the WRITE itself rejects (TypeError "Decompression failed.")
// and both sides error — no read demand required to surface the failure.

import { strictEqual, rejects } from 'node:assert';

const enc = new TextEncoder();

const check = (err) => {
  strictEqual(err.constructor, TypeError);
  strictEqual(err.message, 'Decompression failed.');
  return true;
};

export const invalidDataRejectsWrite = {
  async test() {
    const { writable, readable } = new DecompressionStream('deflate');
    const writer = writable.getWriter();
    const reader = readable.getReader();
    await rejects(writer.write(enc.encode('not compressed data')), check);
    // Both sides are errored; reads keep rejecting.
    await rejects(reader.read(), TypeError);
    await rejects(reader.read(), TypeError);
  },
};

export const invalidDataErrorsIteration = {
  async test() {
    const { writable, readable } = new DecompressionStream('deflate');
    const writer = writable.getWriter();
    const writePromise = writer
      .write(enc.encode('not compressed data'))
      .catch(() => {});
    await rejects(
      (async () => {
        for await (const chunk of readable) {
          void chunk;
        }
      })(),
      TypeError
    );
    await writePromise;
  },
};

export const invalidMagicBytesReject = {
  async test() {
    const { writable, readable } = new DecompressionStream('gzip');
    const writer = writable.getWriter();
    const reader = readable.getReader();
    await rejects(
      writer.write(Uint8Array.of(0x00, 0x01, 0x02, 0x03, 0x04, 0x05)),
      check
    );
    await rejects(reader.read(), TypeError);
  },
};
