// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextDecoderStream state across chunk boundaries: BOM handling and
// incomplete multibyte sequences at end of stream (non-fatal mode).

import { deepStrictEqual } from 'node:assert';

// Writes the given byte chunks (awaiting each), closes, and returns the
// decoded string chunks with enqueue boundaries preserved.
async function collectDecoded(stream, writes) {
  const writer = stream.writable.getWriter();
  const reader = stream.readable.getReader();
  const chunks = [];
  const drained = (async () => {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
    }
  })();
  for (const bytes of writes) {
    await writer.write(Uint8Array.from(bytes));
  }
  await writer.close();
  await drained;
  return chunks;
}

export const bomSplitAcrossWritesIsStripped = {
  async test() {
    const chunks = await collectDecoded(new TextDecoderStream(), [
      [0xef],
      [0xbb],
      [0xbf, 0x41],
    ]);
    deepStrictEqual(chunks, ['A']);
  },
};

export const bomPreservedWithIgnoreBOM = {
  async test() {
    const chunks = await collectDecoded(
      new TextDecoderStream('utf-8', { ignoreBOM: true }),
      [[0xef, 0xbb, 0xbf, 0x41]]
    );
    deepStrictEqual(chunks, ['\uFEFFA']);
  },
};

export const incompleteSequenceReplacedAtClose = {
  async test() {
    // 0xE4 0xB8 is an incomplete 3-byte sequence: 'A' is delivered when
    // written, the replacement character arrives from the close-time flush.
    const chunks = await collectDecoded(new TextDecoderStream(), [
      [0x41, 0xe4, 0xb8],
    ]);
    deepStrictEqual(chunks, ['A', '\uFFFD']);
  },
};
