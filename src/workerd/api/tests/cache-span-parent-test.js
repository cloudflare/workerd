// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

export default {
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

// cacheApiOutbound re-enters this service through an ordinary subrequest channel.
export class Backend extends WorkerEntrypoint {
  async fetch(request) {
    const name = new URL(request.url).pathname.split('/')[1];
    strictEqual(
      request.headers.get('CF-Cache-Namespace'),
      name === 'named' ? name : null
    );
    await request.arrayBuffer();
    return new Response(request.method === 'GET' ? 'cached' : null, {
      headers: { 'CF-Cache-Status': 'HIT' },
    });
  }
}
