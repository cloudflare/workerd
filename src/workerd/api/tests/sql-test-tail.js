// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

import {
  invocationPromises,
  spans,
  testTailHandler,
} from 'test:instrumentation-tail';

export default testTailHandler;

// Expected status and exceptions of the span of a failed operation.
function failed(message, name = 'Error') {
  return {
    status: { code: 'error', message },
    exceptions: [{ name, message }],
  };
}

function countByName(spans) {
  return spans.reduce((acc, curr) => {
    if (!acc[curr.name]) acc[curr.name] = 0;
    acc[curr.name]++;
    return acc;
  }, {});
}

export const test = {
  async test(ctrl, env, ctx) {
    await Promise.allSettled(invocationPromises);
    let received = [...spans.values()];

    assert.deepStrictEqual(countByName(received), {
      durable_object_storage_exec: 296,
      durable_object_storage_ingest: 1030,
      durable_object_storage_getDatabaseSize: 3,
      durable_object_storage_put: 18,
      durable_object_storage_get: 18,
      durable_object_storage_transaction: 8,
      durable_object_subrequest: 48,
      // Inner spans nested under durable_object_subrequest: jsRpcSession for RPC
      // dispatches (one per call, client-side only -- the server's jsrpc-typed
      // onset is its equivalent), fetch for fetch dispatches.
      jsRpcSession: 36,
      fetch: 12,
      // jsRpcCall: 36 client-side + 36 server-side.
      jsRpcCall: 72,
      durable_object_storage_deleteAll: 1,
      createStringTable: 4,
      runActorFunc: 4,
      durable_object_storage_sync: 4,
      getStringTableIds: 4,
      testMultiStatement: 1,
      testRollbackKvInit: 1,
      testRollbackAlarmInit: 1,
      testCursorUaf: 1,
      durable_object_storage_setAlarm: 2,
      durable_object_storage_getAlarm: 1,
      testSessionsAPIBookmark: 20,
      durable_object_storage_getCurrentBookmark: 20,
      durable_object_storage_waitForBookmark: 19,
    });

    // Failed operations, mostly errors that sql-test.js expects.
    assert.deepStrictEqual(
      countByName(received.filter((span) => span.status?.code === 'error')),
      {
        durable_object_storage_exec: 39,
        durable_object_storage_ingest: 3,
        durable_object_storage_transaction: 1,
      }
    );

    const spanForQuery = (query) =>
      received.find((span) => span['db.query.text'] === query);

    // Fails when exec() starts the query.
    assert.deepStrictEqual(spanForQuery('SELECT * FROM spanStatusMissing'), {
      name: 'durable_object_storage_exec',
      'db.system.name': 'cloudflare-durable-object-sql',
      'db.operation.name': 'exec',
      'db.query.text': 'SELECT * FROM spanStatusMissing',
      'cloudflare.durable_object.query.bindings': 0n,
      ...failed('no such table: spanStatusMissing: SQLITE_ERROR'),
      closed: true,
    });

    // Fails while iterating over the results.
    const iterationQuery =
      "SELECT json(column1) AS value FROM (VALUES ('1'), ('not json'))";
    assert.deepStrictEqual(spanForQuery(iterationQuery), {
      name: 'durable_object_storage_exec',
      'db.system.name': 'cloudflare-durable-object-sql',
      'db.operation.name': 'exec',
      'db.query.text': iterationQuery,
      'cloudflare.durable_object.query.bindings': 0n,
      'cloudflare.durable_object.response.rows_read': 0n,
      'cloudflare.durable_object.response.rows_written': 0n,
      ...failed('malformed JSON: SQLITE_ERROR'),
      closed: true,
    });
  },
};
