// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

export const statusResponses = {
  async test(_controller, env) {
    for (const status of [200, 399, 400, 404, 409, 503, 599]) {
      const response = await env.target.fetch(`http://origin/status/${status}`);
      assert.equal(response.status, status);
      assert.equal(await response.text(), 'response');
    }
  },
};
