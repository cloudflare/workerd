import * as assert from 'node:assert';

let invocationPromises = [];
let spans = new Map();

export default {
  async test(ctrl, env, ctx) {
    const expected = [
      {
        name: 'durable_object_storage_put',
        closed: true,
        subrequests: [{ name: 'durable_object_subrequest' }],
      },
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
    ];

    await Promise.allSettled(invocationPromises);
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );
    assert.deepStrictEqual(received, expected);
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
      switch (event.event.type) {
        case 'spanOpen':
          if (event.event.name === 'durable_object_subrequest') {
            let span = spans.get(event.event.spanId);
            span['subrequests'] = span['subrequests']
              ? (span['subrequests'].push({ name: { ...event.name } }),
                span['subrequests'])
              : [{ name: event.event.name }];
          } else {
            // The span ids will change between tests, but Map preserves insertion order
            spans.set(event.event.spanId, { name: event.event.name });
          }
          break;
        case 'attributes': {
          let span = spans.get(event.event.spanId);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          spans.set(event.event.spanId, span);
          break;
        }
        case 'spanClose': {
          const spanId = event.spanContext.spanId;
          let span = spans.get(spanId);
          span['closed'] = true;
          spans.set(spanId, span);
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};
