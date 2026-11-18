// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Workers Caching, for cluster-test.mjs: a GET answers an id of its own run,
// cached for an hour under its path's first segment as a tag; POST /purge
// purges what its JSON body names.
export default {
  async fetch(request, env, ctx) {
    if (request.method === 'POST') {
      return Response.json(await ctx.cache.purge(await request.json()));
    }
    const tag = new URL(request.url).pathname.split('/')[1];
    const headers = { 'cache-control': 'max-age=3600', 'cache-tag': tag };
    return new Response(crypto.randomUUID(), { headers });
  },
};
