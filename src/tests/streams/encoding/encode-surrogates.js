// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextEncoderStream surrogate handling: a lone trailing high surrogate is
// held and paired with the next chunk; unpairable surrogates become U+FFFD
// (EF BF BD); a pending high surrogate at close is replaced on flush. The
// held code unit is prepended to the NEXT chunk's text, so the pair (or
// replacement) is delivered inside that chunk's single enqueue.

import { deepStrictEqual } from 'node:assert';

// Writes the given chunks (awaiting each), closes, and returns the encoded
// chunks as byte arrays with enqueue boundaries preserved.
async function collectEncoded(writes) {
  const tes = new TextEncoderStream();
  const writer = tes.writable.getWriter();
  const reader = tes.readable.getReader();
  const chunks = [];
  const drained = (async () => {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push([...value]);
    }
  })();
  for (const chunk of writes) {
    await writer.write(chunk);
  }
  await writer.close();
  await drained;
  return chunks;
}

export const surrogatePairSplitAcrossWrites = {
  async test() {
    deepStrictEqual(await collectEncoded(['\uD83D', '\uDE00']), [
      [0xf0, 0x9f, 0x98, 0x80], // 😀
    ]);
  },
};

export const loneHighBeforeBmpBecomesReplacement = {
  async test() {
    deepStrictEqual(await collectEncoded(['\uD83D', 'A']), [
      [0xef, 0xbf, 0xbd, 0x41],
    ]);
  },
};

export const loneLowSurrogateBecomesReplacement = {
  async test() {
    deepStrictEqual(await collectEncoded(['\uDC00']), [[0xef, 0xbf, 0xbd]]);
  },
};

export const pendingHighAtCloseFlushesReplacement = {
  async test() {
    deepStrictEqual(await collectEncoded(['\uD83D']), [[0xef, 0xbf, 0xbd]]);
  },
};
