// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Caller worker for binding-span-enrichment tests. The test just triggers the RPC; assertions
// about what the STW received live in binding-span-enrichment-tail.js's `validate` test.

import assert from 'node:assert';

export const callEnrichesSpan = {
  async test(ctrl, env) {
    const result = await env.callee.run('text-embedding-3-small');
    assert.deepStrictEqual(result, { answer: 42 });
  },
};
