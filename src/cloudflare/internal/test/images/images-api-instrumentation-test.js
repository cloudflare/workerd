// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// tailStream is going to be invoked multiple times, but we want to wait
// to run the test until all executions are done. Collect promises for
// each
let invocationPromises = [];
let spans = new Map();

let resolved = 0;

export default {
  tailStream(event, env, ctx) {
    console.log(event);

    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    console.log('START');

    // Accumulate the span info for easier testing
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
          console.log('RESOLVE');
          resolveFn();
          resolved++;
          break;
      }
    };
  },
};

export const test = {
  async test() {
    console.log(invocationPromises);

    // Wait for all the tailStream executions to finish
    await Promise.allSettled(invocationPromises);

    console.log('RESOLVED');

    // Recorded streaming tail worker events, in insertion order,
    // filtering spans not associated with KV
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );

    console.log(received);

    // spans emitted by kv-test.js in execution order
    let expected = [
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        'cloudflare.kv.query.parameter.cacheTtl': 100n,
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        'cloudflare.kv.query.parameter.type': 'json',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        'cloudflare.kv.query.parameter.type': 'json',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        'cloudflare.kv.query.parameter.type': 'arrayBuffer',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        'cloudflare.kv.query.parameter.type': 'banana',
        closed: true,
      },
      {
        name: 'kv_getWithMetadata',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'getWithMetadata',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        'cloudflare.kv.query.parameter.type': 'json',
        closed: true,
      },
      {
        name: 'kv_get_bulk',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get_bulk',
        'cloudflare.kv.query.parameter.type': 'json',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get',
        'cloudflare.kv.query.parameter.type': 'json',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get',
        'cloudflare.kv.query.parameter.type': 'stream',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system': 'cloudflare-kv',
        'cloudflare.kv.operation.name': 'get',
        'cloudflare.kv.query.parameter.type': 'arrayBuffer',
        closed: true,
      },
    ];

    assert.deepStrictEqual(received, expected);
  },
};
