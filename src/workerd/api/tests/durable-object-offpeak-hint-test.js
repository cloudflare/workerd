// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export class OffpeakHintObject {
  fetch() {
    return new Response('ok');
  }
}

// Each worker in the config says whether its compatibility flags allow the "offpeak" location
// hint. Other location hints are accepted either way.
export const offpeakHintMatchesFlags = {
  async test(ctrl, env) {
    const id = env.ns.idFromName('test');
    const getters = [
      () => env.ns.get(id, { locationHint: 'offpeak' }),
      () => env.ns.getByName('test', { locationHint: 'offpeak' }),
    ];
    for (const get of getters) {
      if (env.offpeakAllowed) {
        const response = await get().fetch('http://example.test/');
        assert.strictEqual(await response.text(), 'ok');
      } else {
        assert.throws(get, {
          name: 'TypeError',
          message: 'get called with an unsupported locationHint: "offpeak"',
        });
      }
    }

    const stub = env.ns.get(id, { locationHint: 'wnam' });
    const response = await stub.fetch('http://example.test/');
    assert.strictEqual(await response.text(), 'ok');
  },
};
