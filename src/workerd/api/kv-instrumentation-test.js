// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// tailStream is going to be invoked multiple times, but we want to wait
// to run the test until all executions are done. Collect promises for
// each
let invocationPromises = [];
let spans = new Map();
let topLevelSpanIds = new Map(); // Track top-level span IDs per invocation

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

    // Store the top-level span ID from the onset event
    if (event.event.type === 'onset' && event.event.spanId) {
      topLevelSpanIds.set(event.invocationId, event.event.spanId);
    }

    // Accumulate the span info for easier testing
    return (event) => {
      // For spanOpen events, the new span ID is in event.event.spanId
      // For other events, they reference an existing span via event.spanContext.spanId
      let spanKey;
      switch (event.event.type) {
        case 'spanOpen':
          // spanOpen creates a new span with ID in event.event.spanId
          spanKey = event.invocationId + event.event.spanId;
          spans.set(spanKey, {
            name: event.event.name,
          });
          break;
        case 'attributes': {
          // Filter out top-level attributes events (jsRpcSession span)
          const topLevelSpanId = topLevelSpanIds.get(event.invocationId);
          if (topLevelSpanId && event.spanContext.spanId === topLevelSpanId) {
            // Ignore attributes for the top-level span
            break;
          }

          // attributes references an existing span via spanContext.spanId
          spanKey = event.invocationId + event.spanContext.spanId;
          let span = spans.get(spanKey);
          if (!span) {
            throw new Error(`Attributes event for unknown span: ${spanKey}`);
          }
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          break;
        }
        case 'spanClose': {
          // Filter out top-level spanClose events (jsRpcSession span)
          const topLevelSpanId = topLevelSpanIds.get(event.invocationId);
          if (topLevelSpanId && event.spanContext.spanId === topLevelSpanId) {
            // Ignore spanClose for the top-level span
            break;
          }

          // spanClose references an existing span via spanContext.spanId
          spanKey = event.invocationId + event.spanContext.spanId;
          let span = spans.get(spanKey);
          if (!span) {
            throw new Error(`SpanClose event for unknown span: ${spanKey}`);
          }
          span['closed'] = true;
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

    // Recorded streaming tail worker events, in insertion order,
    // filtering spans not associated with KV
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );

    // spans emitted by kv-test.js in execution order
    let expected = [
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          closed: true,
        },
        name: 'span_0',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          closed: true,
        },
        name: 'span_1',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          closed: true,
        },
        name: 'span_2',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          closed: true,
        },
        name: 'span_3',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          'cloudflare.kv.query.cache_ttl': 100n,
          closed: true,
        },
        name: 'span_4',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          closed: true,
        },
        name: 'span_5',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          'cloudflare.kv.query.type': 'json',
          closed: true,
        },
        name: 'span_6',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          'cloudflare.kv.query.type': 'json',
          closed: true,
        },
        name: 'span_7',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          'cloudflare.kv.query.type': 'arrayBuffer',
          closed: true,
        },
        name: 'span_8',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          'cloudflare.kv.query.type': 'banana',
          closed: true,
        },
        name: 'span_9',
      },
      {
        expected: {
          name: 'kv_getWithMetadata',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'getWithMetadata',
          'cloudflare.kv.query.key': 'key1',
          'cloudflare.kv.response.metadata': true,
          closed: true,
          'cloudflare.kv.response.cache_status': 'HIT',
        },
        name: 'span_10',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          'cloudflare.kv.query.keys': 'key1',
          closed: true,
        },
        name: 'span_11',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          'cloudflare.kv.query.type': 'json',
          'cloudflare.kv.query.keys': 'key1',
          closed: true,
        },
        name: 'span_12',
      },
      {
        expected: {
          name: 'kv_get_bulk',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get_bulk',
          'cloudflare.kv.query.type': 'json',
          'cloudflare.kv.query.keys': 'key1, key2',
          closed: true,
        },
        name: 'span_13',
      },
      {
        expected: {
          name: 'kv_get',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get',
          'cloudflare.kv.response.metadata': true,
          closed: true,
          'cloudflare.kv.response.cache_status': 'HIT',
        },
        name: 'span_14',
      },
      {
        expected: {
          name: 'kv_get',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get',
          closed: true,
        },
        name: 'span_15',
      },
      {
        expected: {
          name: 'kv_get',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get',
          closed: true,
        },
        name: 'span_16',
      },
      {
        expected: {
          name: 'kv_get',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get',
          'cloudflare.kv.response.metadata': true,
          closed: true,
          'cloudflare.kv.response.cache_status': 'HIT',
        },
        name: 'span_17',
      },
      {
        expected: {
          name: 'kv_get',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get',
          'cloudflare.kv.query.type': 'json',
          'cloudflare.kv.response.metadata': true,
          closed: true,
          'cloudflare.kv.response.cache_status': 'HIT',
        },
        name: 'span_18',
      },
      {
        expected: {
          name: 'kv_get',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get',
          'cloudflare.kv.query.type': 'stream',
          'cloudflare.kv.response.metadata': true,
          'cloudflare.kv.response.cache_status': 'HIT',
          closed: true,
        },
        name: 'span_19',
      },
      {
        expected: {
          name: 'kv_get',
          'db.system': 'cloudflare-kv',
          'db.operation.name': 'get',
          'cloudflare.kv.query.type': 'arrayBuffer',
          'cloudflare.kv.response.metadata': true,
          closed: true,
          'cloudflare.kv.response.cache_status': 'HIT',
        },
        name: 'span_20',
      },
      {
        expected: {
          name: 'kv_list',
          'cloudflare.binding_type': 'KV',
          'db.operation.name': 'list',
          'cloudflare.kv.query.prefix': 'te',
          'db.namespace': 'KV',
          'db.system': 'cloudflare-kv',
          closed: true,
          'cloudflare.kv.response.returned_rows': 3n,
          'cloudflare.kv.response.cache_status': 'HIT',
          'cloudflare.kv.response.list_complete': false,
          'cloudflare.kv.response.cursor': '6Ck1la0VxJ0djhidm1MdX2FyD',
          'cloudflare.kv.response.expiration': 1234n,
        },
        name: 'span_21',
      },
      {
        expected: {
          name: 'kv_list',
          'db.system': 'cloudflare-kv',
          'cloudflare.binding_type': 'KV',
          'db.operation.name': 'list',
          'cloudflare.kv.query.cursor': '123',
          'cloudflare.kv.query.limit': 100n,
          'cloudflare.kv.query.prefix': 'te',
          'cloudflare.kv.response.returned_rows': 100n,
          'db.namespace': 'KV',
          closed: true,
          'cloudflare.kv.response.cache_status': 'HIT',
          'cloudflare.kv.response.list_complete': true,
          'cloudflare.kv.response.expiration': 1234n,
        },
        name: 'span_22',
      },
      {
        expected: {
          name: 'kv_list',
          closed: true,
          'cloudflare.binding_type': 'KV',
          'db.operation.name': 'list',
          'cloudflare.kv.query.prefix': 'not-found',
          'cloudflare.kv.response.returned_rows': 0n,
          'db.namespace': 'KV',
          'db.system': 'cloudflare-kv',
          'cloudflare.kv.response.cache_status': 'HIT',
          'cloudflare.kv.response.list_complete': true,
        },
        name: 'span_23',
      },
      {
        expected: {
          name: 'kv_put',
          'cloudflare.kv.query.key': 'foo_with_exp',
          'db.operation.name': 'put',
          'db.system': 'cloudflare-kv',
          'cloudflare.kv.query.value_type': 'text',
          'cloudflare.kv.query.expiration': 10n,
          'cloudflare.kv.query.payload.size': 4n,
          closed: true,
        },
        name: 'span_24',
      },
      {
        expected: {
          name: 'kv_put',
          'cloudflare.kv.query.key': 'foo_with_expTtl',
          'db.operation.name': 'put',
          'db.system': 'cloudflare-kv',
          'cloudflare.kv.query.value_type': 'text',
          'cloudflare.kv.query.expiration_ttl': 15n,
          'cloudflare.kv.query.payload.size': 23n,
          closed: true,
        },
        name: 'span_25',
      },
      {
        expected: {
          name: 'kv_put',
          'cloudflare.kv.query.key': 'foo_with_expTtl',
          'db.operation.name': 'put',
          'db.system': 'cloudflare-kv',
          'cloudflare.kv.query.value_type': 'ArrayBuffer',
          'cloudflare.kv.query.expiration_ttl': 15n,
          'cloudflare.kv.query.payload.size': 256n,
          closed: true,
        },
        name: 'span_26',
      },
    ];

    assert.equal(
      received.length,
      expected.length,
      `Expected ${expected.length} received ${received.length} spans`
    );
    let errors = [];
    for (let i = 0; i < received.length; i++) {
      try {
        assert.deepStrictEqual(received[i], expected[i].expected);
      } catch (e) {
        console.error(`${expected[i].name} does not match`);
        console.log(e);
        errors.push(e);
      }
    }
    if (errors.length > 0) {
      throw 'kv spans are incorrect';
    }
  },
};
