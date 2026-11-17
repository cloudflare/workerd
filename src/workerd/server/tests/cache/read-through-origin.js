// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Worker behind read-through-test.wd-test's cache. Each run of a path
// answers its path and the count of runs, cached for a minute under the tag
// `all`, except as the path asks. `/runs` and `POST /purge` are not counted.
let runs = 0;

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname === '/runs') {
      return new Response(String(runs), {
        headers: { 'Cache-Control': 'no-store' },
      });
    }
    if (url.pathname === '/purge') {
      return Response.json(await ctx.cache.purge(await request.json()));
    }
    runs++;
    const headers = { 'Cache-Control': 'max-age=60', 'Cache-Tag': 'all' };
    switch (url.pathname) {
      case '/slow':
        await scheduler.wait(200);
        break;
      case '/no-store':
        headers['Cache-Control'] = 'no-store';
        break;
      case '/cdn':
        headers['Cache-Control'] = 'no-store';
        headers['CDN-Cache-Control'] = 'max-age=60';
        break;
      case '/short':
        headers['Cache-Control'] = 'max-age=1';
        break;
    }
    if (url.pathname.startsWith('/tagged')) headers['Cache-Tag'] = 'one, Two';
    const body = `${request.method} ${url.pathname}${url.search} ${runs}`;
    return new Response(body, { headers });
  },
};
