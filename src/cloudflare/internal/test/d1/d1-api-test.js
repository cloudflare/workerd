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

export const testInnerFetcherIsPrivate = {
  test(_ctr, env) {
    // User code must reach the inner service stub only through the D1 API.
    const session = env.d1.withSession();
    const statement = env.d1.prepare('select 1');
    assert.deepStrictEqual(Object.keys(env.d1), []);
    assert.deepStrictEqual(Object.keys(session), []);
    assert.deepStrictEqual(Object.keys(statement), ['statement', 'params']);
    assert.strictEqual(env.d1.fetcher, undefined);
    assert.strictEqual(env.d1.alwaysPrimarySession, undefined);
    assert.strictEqual(session.fetcher, undefined);
    assert.strictEqual(statement.dbSession, undefined);
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
      bookmark: response.bookmark,
      results: [
        {
          meta: response.results[0].meta,
          data: {
            kind: 'raw',
            columns: ['answer'],
            rows: [[42]],
          },
        },
      ],
    });
    assert.equal(typeof response.bookmark, 'string');
    assert.equal(typeof response.results[0].meta.duration, 'number');
  },
};
