// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The store of r2-test's `PROBED` bucket: `BLOBS`, with the HEADs that
// reach it counted and the HEAD of one object refused. Requests to
// `http://control/` set it: a POST names the object, whose HEAD is then a
// 500, and starts the count again; a GET answers `{ heads }`.

let heads = 0;
let failing = null;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.hostname === 'control') {
      if (request.method === 'POST') {
        failing = await request.text();
        heads = 0;
      }
      return Response.json({ heads });
    }
    if (request.method === 'HEAD') {
      heads++;
      // The path is `<bucket>/<key>`, percent-encoded whole.
      const path = decodeURIComponent(url.pathname.slice(1));
      if (path.slice(path.indexOf('/') + 1) === failing) {
        return new Response(null, { status: 500 });
      }
    }
    return env.BLOBS.fetch(request);
  },
};
