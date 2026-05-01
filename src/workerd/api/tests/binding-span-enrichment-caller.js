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

// Drives runEdgeCases on the callee. The actual assertions on what the STW saw live in
// the validateEdgeCases test in binding-span-enrichment-tail.js.
export const callEdgeCases = {
  async test(ctrl, env) {
    const result = await env.callee.runEdgeCases();
    assert.strictEqual(result, 'ok');
  },
};
