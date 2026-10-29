// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

export const testAsyncStackTrace = {
  async test(_controller, env) {
    try {
      await env.unreachable.fetch('http://origin/unreachable');
      assert.fail('fetch should reject');
    } catch (e) {
      assert.equal(e.message, 'Network connection lost.');
      assert.ok(e.stack.includes('at async Object.test'), e.stack);
    }
  },
};
