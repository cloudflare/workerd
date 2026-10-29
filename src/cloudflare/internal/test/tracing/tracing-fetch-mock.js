// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/start') {
      return Response.redirect('http://origin/middle?hop=2', 302);
    }
    if (url.pathname === '/middle') {
      return Response.redirect('http://other/final?hop=3', 307);
    }
    const status = Number(url.pathname.split('/')[2]) || 200;
    return new Response('response', { status });
  },
};
