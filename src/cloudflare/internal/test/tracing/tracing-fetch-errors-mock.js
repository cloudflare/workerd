// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/abort') await scheduler.wait(10);
    if (url.pathname === '/loop') {
      return Response.redirect(
        `http://origin/loop?n=${Number(url.searchParams.get('n')) + 1}`,
        302
      );
    }
    if (url.pathname === '/bad-location') {
      return new Response(null, {
        status: 302,
        headers: { location: 'http://[' },
      });
    }
    if (url.pathname === '/redirect-bad') {
      return Response.redirect('http://origin/bad-location', 302);
    }
    if (url.pathname === '/manual') {
      return Response.redirect('http://origin/success', 302);
    }
    return new Response('response', {
      status: Number(url.pathname.split('/')[2]) || 200,
    });
  },
};
