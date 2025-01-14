// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const tests = {
  async test(_, env) {
    {
      // Test legacy fetch
      const resp = await env.browser.fetch('http://workers-binding.brapi/run', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          inputs: { test: true },
        }),
      });
      assert.deepStrictEqual(await resp.json(), { success: true });
    }
  },
};
