// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

export default {
  // Runs match, put, and delete on the default cache and on a named cache.
  // cache-span-parent-test-tail.js checks the trace of each operation.
  async test() {
    for (const name of ['default', 'named']) {
      const cache =
        name === 'default' ? caches.default : await caches.open(name);
      const prefix = `https://cache.test/${name}`;
      const response = await cache.match(`${prefix}/match`);
      strictEqual(await response.text(), 'cached');
      await cache.put(
        `${prefix}/put`,
        new Response('cached', {
          headers: { 'Cache-Control': 'max-age=60' },
        })
      );
      strictEqual(await cache.delete(`${prefix}/delete`), true);
    }
  },
};

// cacheApiOutbound re-enters this service through an ordinary subrequest channel. Backend stands
// in for the cache, and answers every operation successfully.
export class Backend extends WorkerEntrypoint {
  async fetch(request) {
    // Only named caches send CF-Cache-Namespace.
    const name = new URL(request.url).pathname.split('/')[1];
    strictEqual(
      request.headers.get('CF-Cache-Namespace'),
      name === 'named' ? name : null
    );
    // put() doesn't finish until Backend reads its body, the serialized response.
    await request.arrayBuffer();
    return new Response(request.method === 'GET' ? 'cached' : null, {
      headers: { 'CF-Cache-Status': 'HIT' },
    });
  }
}
