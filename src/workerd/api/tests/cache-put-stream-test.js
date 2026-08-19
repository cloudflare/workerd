// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export const concurrentClonePuts = {
  async test() {
    const { readable, writable } = new TransformStream();
    const response = new Response(readable);
    const clone = response.clone();

    const puts = Promise.all([
      caches.default.put('https://example.com/clone-1', response),
      caches.default.put('https://example.com/clone-2', clone),
    ]);

    const writer = writable.getWriter();
    const write = (async () => {
      const chunk = new Uint8Array(64 * 1024);
      for (let i = 0; i < 16; ++i) {
        await writer.write(chunk);
      }
      await writer.close();
    })();

    const result = await Promise.race([
      Promise.all([puts, write]).then(() => 'completed'),
      scheduler.wait(5000).then(() => 'timed out'),
    ]);
    assert.strictEqual(result, 'completed');
  },
};
