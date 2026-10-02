// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

const syntaxErrorMessage =
  'near "INVALID": syntax error at offset 0: SQLITE_ERROR';

export const testDirectQuerySuccess = {
  async test(_ctr, env) {
    const response = await env.d1.query({
      queries: [
        { sql: 'select ? as answer', params: [42] },
        { sql: 'select ? as answer', params: [43] },
      ],
    });
    assert.deepEqual(Object.keys(response), ['results']);
    assert.deepEqual(
      response.results.map((result) => result.data),
      [
        { kind: 'raw', columns: ['answer'], rows: [[42]] },
        { kind: 'raw', columns: ['answer'], rows: [[43]] },
      ]
    );
    for (const result of response.results) {
      assert.equal(typeof result.meta.duration, 'number');
      assert.equal(result.meta.served_by_colo, 'DFW');
    }
  },
};

export const testLegacyQuerySuccess = {
  async test(_ctr, env) {
    const bookmark = 'token-legacy';
    for (const route of [
      `/commitTokens/nextToken?t=${bookmark}`,
      '/commitTokens/legacyResponse',
    ]) {
      const response = await env.d1MockFetcher.fetch(
        `http://d1-api-test${route}`
      );
      assert.equal(response.status, 200);
    }

    const response = await env.d1.query({
      queries: [{ sql: 'select ? as answer', params: [42] }],
      bookmark: 'first-primary',
    });
    assert.deepEqual(Object.keys(response), ['results', 'bookmark']);
    assert.deepEqual(
      response.results.map((result) => result.data),
      [{ kind: 'raw', columns: ['answer'], rows: [[42]] }]
    );
    assert.equal(response.bookmark, bookmark);
  },
};

export const testThrownErrorsPropagate = {
  async test(_ctr, env) {
    const db = env.d1;
    // query() rejects with the RPC error. It does not add a D1 error wrapper.
    await assert.rejects(
      () => db.query({ queries: [{ sql: 'INVALID SQL' }] }),
      (error) => {
        assertBackendError(error);
        return true;
      }
    );

    // These methods add a D1 error wrapper. Its cause must keep the RPC error.
    for (const operation of [
      () => db.prepare('INVALID SQL').all(),
      () => db.batch([db.prepare('INVALID SQL')]),
    ]) {
      await assert.rejects(operation, (error) => {
        assert.equal(error.message, `D1_ERROR: Error: ${syntaxErrorMessage}`);
        assertBackendError(error.cause);
        return true;
      });
    }
  },
};

export const testExecPreservesErrorContract = {
  async test(_ctr, env) {
    // exec() keeps its public prefix and the original backend message.
    await assert.rejects(
      () => env.d1.exec('INVALID SQL'),
      (error) => {
        assert.equal(error.message, `D1_EXEC_ERROR: ${syntaxErrorMessage}`);
        assertBackendError(error.cause);
        return true;
      }
    );
  },
};

function assertBackendError(error) {
  assert.ok(error instanceof Error);
  assert.equal(error.message, syntaxErrorMessage);
  assert.deepEqual(error.metadata, { code: 'D1_TEST_ERROR', queryIndex: 0 });
  assert.ok(error.cause instanceof Error);
  assert.equal(error.cause.message, syntaxErrorMessage);
}
