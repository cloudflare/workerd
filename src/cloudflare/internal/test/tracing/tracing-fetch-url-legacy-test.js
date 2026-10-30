// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

export const relativeUrl = {
  async test(_controller, env) {
    const response = await env.ns
      .get(env.ns.idFromName('relative'))
      .fetch('/relative?q=1#f');
    assert.equal(await response.text(), 'relative response');
  },
};

export class Actor {
  fetch() {
    return new Response('relative response');
  }
}
