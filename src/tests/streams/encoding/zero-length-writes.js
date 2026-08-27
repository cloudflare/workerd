// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Zero-length chunks resolve without enqueuing anything: no empty strings
// or empty byte chunks are ever delivered.

import { deepStrictEqual } from 'node:assert';

async function collect(stream, writes, map) {
  const writer = stream.writable.getWriter();
  const reader = stream.readable.getReader();
  const chunks = [];
  const drained = (async () => {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(map(value));
    }
  })();
  for (const chunk of writes) {
    await writer.write(chunk);
  }
  await writer.close();
  await drained;
  return chunks;
}

export const encoderEmptyStringIsNoop = {
  async test() {
    const chunks = await collect(
      new TextEncoderStream(),
      ['', 'A', ''],
      (v) => [...v]
    );
    deepStrictEqual(chunks, [[0x41]]);
  },
};

export const decoderEmptyChunksAreNoops = {
  async test() {
    const chunks = await collect(
      new TextDecoderStream(),
      [new Uint8Array(0), new ArrayBuffer(0), Uint8Array.of(0x42)],
      (v) => v
    );
    deepStrictEqual(chunks, ['B']);
  },
};
