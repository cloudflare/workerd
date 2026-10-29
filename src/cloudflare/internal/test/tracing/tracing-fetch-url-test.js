// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

export const redirects = {
  async test(_controller, env) {
    const response = await env.target.fetch('http://origin/start?hop=1');
    assert.equal(response.url, 'http://other/final?hop=3');
    assert.equal(await response.text(), 'response');
  },
};

export const credentialsAndFragment = {
  async test(_controller, env) {
    const response = await env.target.fetch('http://user:pass@origin/x#f');
    assert.equal(await response.text(), 'response');
  },
};
