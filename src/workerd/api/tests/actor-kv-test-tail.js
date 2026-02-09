import * as assert from 'node:assert';
import {
  invocationPromises,
  spans,
  testTailHandler,
} from 'test:instrumentation-tail';

// Use shared instrumentation test tail worker
export default testTailHandler;

export const test = {
  async test(ctrl, env, ctx) {
    const expected = [
      { name: 'durable_object_storage_put', closed: true },
      { name: 'durable_object_storage_put', closed: true },
      { name: 'durable_object_storage_get', closed: true },
      { name: 'durable_object_storage_get', closed: true },
      { name: 'durable_object_storage_delete', closed: true },
      { name: 'durable_object_storage_list', closed: true },
      { name: 'durable_object_storage_deleteAll', closed: true },
      { name: 'durable_object_storage_setAlarm', closed: true },
      { name: 'durable_object_storage_getAlarm', closed: true },
      { name: 'durable_object_storage_deleteAlarm', closed: true },
      { name: 'durable_object_storage_transaction', closed: true },
      { name: 'durable_object_storage_sync', closed: true },
      {
        name: 'durable_object_subrequest',
        closed: true,
        objectId:
          'aa299662980ce671dbcb09a5d7ab26ab30e45465bcd12f263f2bdd7d5edd804a',
      },
    ];

    await Promise.allSettled(invocationPromises);
    let received = Array.from(spans.values());
    assert.deepStrictEqual(received, expected);
  },
};
