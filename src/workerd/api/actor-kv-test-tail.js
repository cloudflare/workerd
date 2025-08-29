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
    console.log(JSON.stringify(received));
    const expected = [
      { name: 'durable_object_storage_put', closed: true },
      { name: 'durable_object_storage_get', closed: true },
      { name: 'durable_object_storage_delete', closed: true },
      { name: 'durable_object_storage_list', closed: true },
      { name: 'durable_object_storage_deleteAll', closed: true },
      { name: 'durable_object_storage_setAlarm', closed: true },
      { name: 'durable_object_storage_getAlarm', closed: true },
      { name: 'durable_object_storage_deleteAlarm', closed: true },
      { name: 'durable_object_storage_transaction', closed: true },
      { name: 'durable_object_storage_sync', closed: true },
    ];

    await Promise.allSettled(invocationPromises);
    assert.deepStrictEqual(received, expected);

    return new Response('');
  },

  tailStream(event, env, ctx) {
    console.log(event);
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    return (event) => {
      switch (event.event.type) {
        case 'spanOpen':
          // The span ids will change between tests, but Map preserves insertion order
          spans.set(event.spanId, { name: event.event.name });
          break;
        case 'attributes': {
          let span = spans.get(event.spanId);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          spans.set(event.spanId, span);
          break;
        }
        case 'spanClose': {
          let span = spans.get(event.spanId);
          span['closed'] = true;
          spans.set(event.spanId, span);
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};
