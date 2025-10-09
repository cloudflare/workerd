// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// tailStream is going to be invoked multiple times, but we want to wait
// to run the test until all executions are done. Collect promises for
// each
let invocationPromises = [];
let spans = new Map();

export default {
  tailStream(event, env, ctx) {
    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    // Accumulate the span info for easier testing
    return (event) => {
      // span ids are simple counters for tests, but invocation ID allows us to differentiate them
      let spanKey = event.invocationId + event.spanContext.spanId;
      switch (event.event.type) {
        case 'spanOpen':
          console.log(event.invocationId + event.event.spanId);
          console.log(event.event.name);
          spans.set(event.invocationId + event.event.spanId, {
            name: event.event.name,
          });
          break;
        case 'attributes': {
          let span = spans.get(spanKey);
          if (span) {
            for (let { name, value } of event.event.info) {
              span[name] = value;
            }
            spans.set(spanKey, span);
          }
          break;
        }
        case 'spanClose': {
          let span = spans.get(spanKey);
          span['closed'] = true;
          spans.set(spanKey, span);
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};

export const test = {
  async test() {
    // Wait for all the tailStream executions to finish
    await Promise.allSettled(invocationPromises);

    // Recorded streaming tail worker events, in insertion order.
    let received = Array.from(spans.values());
    console.log(received);

    // spans emitted by sync-kv-test.js in execution order
    let expected = [
      { name: 'durable_object_storage_kv_get', closed: true },
      { name: 'durable_object_storage_kv_put', closed: true },
      { name: 'durable_object_storage_kv_get', closed: true },
      { name: 'durable_object_storage_kv_get', closed: true },
      { name: 'durable_object_storage_kv_put', closed: true },
      { name: 'durable_object_storage_kv_get', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_put', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_storage_kv_delete', closed: true },
      { name: 'durable_object_storage_kv_delete', closed: true },
      { name: 'durable_object_storage_kv_list', closed: true },
      { name: 'durable_object_subrequest', closed: true },
    ];

    assert.deepStrictEqual(received, expected);
  },
};
