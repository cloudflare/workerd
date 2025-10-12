import * as assert from 'node:assert';

let invocationPromises = [];
let spans = new Map();

export default {
  async fetch(ctrl, env, ctx) {
    return new Response('');
  },
  async test(ctrl, env, ctx) {
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );

    const reduced = received.reduce((acc, curr) => {
      if (!acc[curr.name]) acc[curr.name] = 0;
      acc[curr.name]++;
      return acc;
    }, {});
    await Promise.allSettled(invocationPromises);
    assert.deepStrictEqual(reduced, {
      durable_object_storage_exec: 268,
      durable_object_storage_ingest: 1031,
      durable_object_storage_getDatabaseSize: 3,
      durable_object_storage_put: 18,
      durable_object_storage_get: 18,
      durable_object_storage_transaction: 8,
      durable_object_subrequest: 46,
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
    return new Response('');
  },

  tailStream(event, env, ctx) {
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    return (event) => {
      try {
        const rpcMethodName = Array.isArray(event.event.info)
          ? (event.event.info || []).find(
              (item) => item['name'] === 'jsrpc.method'
            )?.value
          : undefined;
        const spanUniqueId = `${event.spanContext.traceId}#${event.event.spanId || event.spanContext.spanId}`;
        switch (event.event.type) {
          case 'spanOpen':
            // The span ids will change between tests, but Map preserves insertion order
            spans.set(spanUniqueId, {
              name: event.event.name || rpcMethodName,
            });
            break;
          case 'attributes': {
            let span = spans.get(spanUniqueId) || { name: rpcMethodName };
            for (let { name, value } of event.event.info) {
              span[name] = value;
            }
            spans.set(spanUniqueId, span);
            break;
          }
          case 'spanClose': {
            let span = spans.get(spanUniqueId);
            span['closed'] = true;
            spans.set(spanUniqueId, span);
            break;
          }
          case 'outcome':
            resolveFn();
            break;
        }
      } catch (e) {
        resolveFn();
      }
    };
  },
};
