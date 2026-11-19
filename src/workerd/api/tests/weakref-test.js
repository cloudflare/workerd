// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

async function get(target, query) {
  const response = await target.fetch(`http://target/?${query}`);
  assert.strictEqual(response.status, 200);
  return response;
}

export const finalizationRegistry = {
  async test(ctrl, { frTarget: target }) {
    // The first request doesn't do any I/O so we won't notice the effects of FinalizationRegistry
    // cleanup callbacks as part of the response.
    assert.strictEqual(await (await get(target, 'test=fr')).text(), '0');

    // Subsequent requests do I/O so we will immediately see the effects of FinalizationRegistry
    // cleanup callbacks.
    for (let i = 0; i < 2; ++i) {
      assert.strictEqual(
        await (await get(target, 'test=fr')).text(),
        `${i + 2}`
      );
    }
  },
};

export const weakRef = {
  async test(ctrl, { weakRefTarget: target }) {
    let data = await (await get(target, 'test=weakref&create')).json();
    assert.deepStrictEqual(data, { created: true, value: "I'm alive!" });

    data = await (await get(target, 'test=weakref')).json();
    assert.deepStrictEqual(data, {
      isDereferenced: false,
      value: "I'm alive!",
    });

    data = await (await get(target, 'test=weakref&gc')).json();
    assert.deepStrictEqual(data, { isDereferenced: true, value: null });

    // A new object can be referenced again after the old one was collected.
    data = await (await get(target, 'test=weakref&create')).json();
    assert.deepStrictEqual(data, { created: true, value: "I'm alive!" });
  },
};
