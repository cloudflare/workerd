// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextEncoderStream ToString-coerces written chunks per the Encoding
// Standard's "encode and enqueue a chunk".

import { strictEqual } from 'node:assert';

export const encoderCoercesChunksToString = {
  async test() {
    const decoder = new TextDecoder();
    const enc = new TextEncoderStream();
    const writer = enc.writable.getWriter();
    const reader = enc.readable.getReader();

    for (const [chunk, expected] of [
      [undefined, 'undefined'],
      [1, '1'],
      [{}, '[object Object]'],
    ]) {
      const [, result] = await Promise.all([
        writer.write(chunk),
        reader.read(),
      ]);
      strictEqual(decoder.decode(result.value), expected);
    }

    await writer.close();
  },
};
