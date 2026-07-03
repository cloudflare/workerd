// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

import { testD1ApiQueriesHappyPath, testD1Exec } from './d1-api-test-common';

export const testWithoutSessions = {
  async test(_ctr, env) {
    await testD1ApiQueriesHappyPath(env.d1);
  },
};

export const testExec = {
  async test(_ctr, env) {
    await testD1Exec(env.d1);
  },
};

export const testDirectQuery = {
  async test(_ctr, env) {
    if (!Cloudflare.compatibilityFlags['d1_binding_jsrpc']) {
      return;
    }

    const response = await env.d1.query({
      queries: [{ sql: 'select 42 as answer' }],
      bookmark: 'first-unconstrained',
    });

    assert.deepEqual(response, {
      success: true,
      results: {
        bookmark: response.results.bookmark,
        queryResults: [
          {
            meta: response.results.queryResults[0].meta,
            data: {
              kind: 'raw',
              columns: ['answer'],
              rows: [[42]],
            },
          },
        ],
      },
    });
    assert.equal(typeof response.results.bookmark, 'string');
    assert.equal(typeof response.results.queryResults[0].meta.duration, 'number');
  },
};
