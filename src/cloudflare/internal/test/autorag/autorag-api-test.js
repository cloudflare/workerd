// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const tests = {
  async test(_, env) {
    {
      // Test search method (legacy fetch path)
      const resp = await env.ai
        .autorag('my-rag')
        .search({ query: 'example query' });

      assert.deepEqual(
        resp.data[0].content[0].text,
        'According to the latest regulations, each passenger is allowed to carry up to two woodchucks.'
      );
    }

    {
      // Test ai search method (legacy fetch path)
      const resp = await env.ai
        .autorag('my-rag')
        .aiSearch({ query: 'example query' });

      assert.deepEqual(resp.response, 'this is an example result');
    }

    {
      // Test search method (beta RPC path)
      const resp = await env.ai
        .autorag('my-rag', { beta: true })
        .search({ query: 'example query' });

      assert.deepEqual(
        resp.data[0].content[0].text,
        'According to the latest regulations, each passenger is allowed to carry up to two woodchucks.'
      );
    }

    {
      // Test ai search method (beta RPC path)
      const resp = await env.ai
        .autorag('my-rag', { beta: true })
        .aiSearch({ query: 'example query' });

      assert.deepEqual(resp.response, 'this is an example result');
    }
  },
};
