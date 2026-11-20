// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Cache API, for cluster-test.mjs: PUT stores the body, GET matches and
// DELETE deletes the entry of the path, in the cache named `?ns=` if given,
// for `?ttl=` seconds if given.
export default {
  async fetch(request) {
    const params = new URL(request.url).searchParams;
    const ns = params.get('ns');
    const cache = ns === null ? caches.default : await caches.open(ns);
    const key = `https://cache.test${new URL(request.url).pathname}`;
    switch (request.method) {
      case 'PUT': {
        const ttl = params.get('ttl') ?? 3600;
        const headers = { 'cache-control': `max-age=${ttl}` };
        await cache.put(key, new Response(request.body, { headers }));
        return new Response(null, { status: 204 });
      }
      case 'DELETE':
        return Response.json(await cache.delete(key));
      default:
        return (await cache.match(key)) ?? new Response(null, { status: 404 });
    }
  },
};
