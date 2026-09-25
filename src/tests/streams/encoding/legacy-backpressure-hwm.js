// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// With fixed TransformStream backpressure but without
// encoder_stream_spec_compliant_backpressure, the readable side has HWM 1:
// the first write settles without read demand (buffered in the readable
// queue), later writes park until a read drains it.

import { strictEqual, deepStrictEqual } from 'node:assert';

function macrotask() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

export const legacyFirstWriteSettlesEagerly = {
  async test() {
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    let first = false;
    let second = false;
    const p1 = writer.write('x').then(() => (first = true));
    const p2 = writer.write('y').then(() => (second = true));
    await macrotask();
    await macrotask();
    strictEqual(first, true);
    strictEqual(second, false);

    const reader = tes.readable.getReader();
    deepStrictEqual([...(await reader.read()).value], [0x78]);
    deepStrictEqual([...(await reader.read()).value], [0x79]);
    await Promise.all([p1, p2]);
    strictEqual(second, true);
    await writer.close();
  },
};
