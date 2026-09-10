// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextDecoderStream with a non-UTF-8 label: the label selects the codec and
// decode state carries across chunk boundaries (byte-at-a-time writes).

import { strictEqual } from 'node:assert';

export const big5StreamingDecode = {
  async test() {
    const dec = new TextDecoderStream('big5');
    const input = [0xa4, 0xa4, 0xb0, 0xea, 0xa4, 0x48];

    const writer = dec.writable.getWriter();
    for (const byte of input) {
      writer.write(new Uint8Array([byte]));
    }
    writer.close();

    let result = '';
    for await (const chunk of dec.readable) {
      result += chunk;
    }

    strictEqual(result, '中國人');
  },
};
