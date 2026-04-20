// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as consumers from 'node:stream/consumers';
import consumersDefault from 'node:stream/consumers';

export const streamConsumersBytesTest = {
  test: async () => {
    assert.strictEqual(typeof consumers.bytes, 'function');
    assert.strictEqual(typeof consumersDefault.bytes, 'function');

    // Quick behavioural smoke: feed a single chunk async iterable and verify
    // we get a Uint8Array back containing the same bytes.
    const chunk = new Uint8Array([1, 2, 3, 4]);
    async function* src() {
      yield chunk;
    }
    const out = await consumers.bytes(src());
    assert.ok(out instanceof Uint8Array);
    assert.deepStrictEqual(Array.from(out), [1, 2, 3, 4]);
  },
};
