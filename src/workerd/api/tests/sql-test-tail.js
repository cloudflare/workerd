import * as assert from 'node:assert';

import {
  invocationPromises,
  spans,
  testTailHandler,
} from 'test:instrumentation-tail';

export default testTailHandler;

export const test = {
  async test(ctrl, env, ctx) {
    await Promise.allSettled(invocationPromises);
    let received = spans.values();

    const reduced = received.reduce((acc, curr) => {
      if (!acc[curr.name]) acc[curr.name] = 0;
      acc[curr.name]++;
      return acc;
    }, {});
    assert.deepStrictEqual(reduced, {
      durable_object_storage_exec: 268,
      durable_object_storage_ingest: 1030,
      durable_object_storage_getDatabaseSize: 3,
      durable_object_storage_put: 18,
      durable_object_storage_get: 18,
      durable_object_storage_transaction: 8,
      durable_object_subrequest: 47,
      durable_object_storage_deleteAll: 1,
      createStringTable: 4,
      runActorFunc: 4,
      durable_object_storage_sync: 4,
      getStringTableIds: 4,
      testMultiStatement: 1,
      testRollbackKvInit: 1,
      testRollbackAlarmInit: 1,
      durable_object_storage_setAlarm: 2,
      durable_object_storage_getAlarm: 1,
      testSessionsAPIBookmark: 20,
    });
  },
};
