// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// With the standard constructor but without fixup-transform-stream-
// backpressure, TransformStream applies no effective backpressure: every
// write settles without read demand.

import { strictEqual, deepStrictEqual } from 'node:assert';

function macrotask() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

export const legacyAllWritesSettleWithoutDemand = {
  async test() {
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    let settled = 0;
    const p1 = writer.write('x').then(() => settled++);
    const p2 = writer.write('y').then(() => settled++);
    await macrotask();
    await macrotask();
    strictEqual(settled, 2);
    await Promise.all([p1, p2]);

    // The codec itself is the standard one: data decodes/encodes normally.
    const reader = tes.readable.getReader();
    deepStrictEqual([...(await reader.read()).value], [0x78]);
    deepStrictEqual([...(await reader.read()).value], [0x79]);
    await writer.close();
  },
};
