// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// The service that mixed.js sends subrequests to.
export default {
  fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/json') {
      return Response.json({ n: Number(url.searchParams.get('n')) });
    }
    return new Response('hello from upstream '.repeat(32));
  },
};
