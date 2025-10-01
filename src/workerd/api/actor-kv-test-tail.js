import * as assert from 'node:assert';
import {
  invocationPromises,
  spans,
  testTailHandler,
} from 'test:instumentation-tail';

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
      { name: 'durable_object_subrequest', closed: true },
    ];

    await Promise.allSettled(invocationPromises);
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );
    assert.deepStrictEqual(received, expected);
    return new Response('');
  },
};
