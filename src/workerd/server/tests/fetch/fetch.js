// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async test(ctrl, env) {
    const response = await env.HANDLER.fetch('http://example.com/');
    const text = await response.text();
    if (response.status !== 200 || text !== 'ok') {
      throw new Error(`unexpected response: ${response.status} ${text}`);
    }
    // A Worker's exception reaches the Worker that called it.
    let threw = false;
    try {
      await env.THROWS.fetch('http://example.com/');
    } catch {
      threw = true;
    }
    if (!threw) throw new Error('the throwing handler did not fail the fetch');
  },
};
