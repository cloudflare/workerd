// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

export default {
  fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/start') {
      return Response.redirect(
        'http://origin/middle?hop=2#middle-fragment',
        302
      );
    }
    if (url.pathname === '/middle') {
      return Response.redirect('http://other/final?hop=3#final-fragment', 307);
    }
    return new Response('response');
  },
};

// Target of the legacy relative-URL fetch, which is made by the inline legacy-fetch-url service.
export class Actor {
  fetch() {
    return new Response('relative response');
  }
}

export const redirects = {
  async test(_controller, env) {
    const response = await env.SELF.fetch('http://origin/start?hop=1');
    assert.equal(response.url, 'http://other/final?hop=3');
    assert.equal(await response.text(), 'response');
  },
};

export const fragment = {
  async test(_controller, env) {
    const response = await env.SELF.fetch('http://origin/x#f');
    assert.equal(await response.text(), 'response');
  },
};
