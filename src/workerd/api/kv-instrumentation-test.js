// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';
import { createInstrumentationState } from 'instrumentation-test-helper';

// KV test uses a custom tail stream handler and test implementation because:
// 1. It uses a different span key format: "invocationId + spanId" (without separator)
//    instead of the standard "invocationId#spanId" format
// 2. It needs custom error handling for unknown spans
// 3. It collects and reports all assertion errors at once rather than failing on first error
// We still use createInstrumentationState for the basic state management
const state = createInstrumentationState();

// Custom handler that uses different span key format and filters top-level spans
const createKVTailStreamHandler = (state) => {
  return (event, env, ctx) => {
    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    state.invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    // Capture the top-level span ID from the onset event
    const topLevelSpanId = event.event.spanId;

    // Accumulate the span info for easier testing
    return (event) => {
      // For spanOpen events, the new span ID is in event.event.spanId
      // For other events, they reference an existing span via event.spanContext.spanId
      let spanKey = event.invocationId + event.spanContext.spanId;
      switch (event.event.type) {
        case 'spanOpen':
          // spanOpen creates a new span with ID in event.event.spanId
          spanKey = event.invocationId + event.event.spanId;
          state.spans.set(spanKey, {
            name: event.event.name,
          });
          break;
        case 'attributes': {
          // Filter out top-level attributes events (jsRpcSession span)
          if (topLevelSpanId && event.spanContext.spanId === topLevelSpanId) {
            // Ignore attributes for the top-level span
            break;
          }

          // attributes references an existing span via spanContext.spanId
          let span = state.spans.get(spanKey);
          if (!span) {
            throw new Error(`Attributes event for unknown span: ${spanKey}`);
          }
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          break;
        }
        case 'spanClose': {
          // spanClose references an existing span via spanContext.spanId
          let span = state.spans.get(spanKey);
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
  };
};

export default {
  tailStream: createKVTailStreamHandler(state),
};

export const test = {
  async test() {
    // Wait for all the tailStream executions to finish
    await Promise.allSettled(state.invocationPromises);

    // Recorded streaming tail worker events, in insertion order,
    // filtering spans not associated with KV
    let received = Array.from(state.spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );

    // spans emitted by kv-test.js in execution order
    let expected = [
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key1, key"2',
        'cloudflare.kv.query.keys.count': 2n,
        'cloudflare.kv.response.size': 85n,
        'cloudflare.kv.response.returned_rows': 2n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key1, key2',
        'cloudflare.kv.query.keys.count': 2n,
        'cloudflare.kv.response.size': 79n,
        'cloudflare.kv.response.returned_rows': 2n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys':
          'key0, key1, key2, key3, key4, key5, key6, key7, key8, key9, key10, key11, key12, key13, key14, key15, key16, key17, key18, key19, key20, key21, key22, key23, key24, key25, key26, key27, key28, key29, key30, key31, key32, key33, key34, key35, key36, key37, key38, key39, key40, key41, key42, key43, key44, key45, key46, key47, key48, key49, key50, key51, key52, key53, key54, key55, key56, key57, key58, key59, key60, key61, key62, key63, key64, key65, key66, key67, key68, key69, key70, key71, key72, key73, k...',
        'cloudflare.kv.query.keys.count': 100n,
        'cloudflare.kv.response.size': 4081n,
        'cloudflare.kv.response.returned_rows': 100n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys':
          'key0, key1, key2, key3, key4, key5, key6, key7, key8, key9, key10, key11, key12, key13, key14, key15, key16, key17, key18, key19, key20, key21, key22, key23, key24, key25, key26, key27, key28, key29, key30, key31, key32, key33, key34, key35, key36, key37, key38, key39, key40, key41, key42, key43, key44, key45, key46, key47, key48, key49, key50, key51, key52, key53, key54, key55, key56, key57, key58, key59, key60, key61, key62, key63, key64, key65, key66, key67, key68, key69, key70, key71, key72, key73, k...',
        'cloudflare.kv.query.keys.count': 101n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key1, not-found',
        'cloudflare.kv.query.keys.count': 2n,
        'cloudflare.kv.query.cache_ttl': 100n,
        'cloudflare.kv.response.size': 57n,
        'cloudflare.kv.response.returned_rows': 2n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': '',
        'cloudflare.kv.query.keys.count': 0n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key1, key2',
        'cloudflare.kv.query.keys.count': 2n,
        'cloudflare.kv.query.type': 'json',
        'cloudflare.kv.response.size': 67n,
        'cloudflare.kv.response.returned_rows': 2n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key-not-json, key2',
        'cloudflare.kv.query.keys.count': 2n,
        'cloudflare.kv.query.type': 'json',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key-not-json, key2',
        'cloudflare.kv.query.keys.count': 2n,
        'cloudflare.kv.query.type': 'arrayBuffer',
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key-not-json, key2',
        'cloudflare.kv.query.keys.count': 2n,
        'cloudflare.kv.query.type': 'banana',
        closed: true,
      },
      {
        name: 'kv_getWithMetadata',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key1',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.metadata': true,
        'cloudflare.kv.response.size': 10n,
        'cloudflare.kv.response.returned_rows': 1n,
        closed: true,
      },
      {
        name: 'kv_getWithMetadata',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key1',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.response.size': 80n,
        'cloudflare.kv.response.returned_rows': 1n,
        closed: true,
      },
      {
        name: 'kv_getWithMetadata',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key1',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.query.type': 'json',
        'cloudflare.kv.response.size': 74n,
        'cloudflare.kv.response.returned_rows': 1n,
        closed: true,
      },
      {
        name: 'kv_getWithMetadata',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'key1, key2',
        'cloudflare.kv.query.keys.count': 2n,
        'cloudflare.kv.query.type': 'json',
        'cloudflare.kv.response.size': 147n,
        'cloudflare.kv.response.returned_rows': 2n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'success',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.metadata': true,
        'cloudflare.kv.response.size': 13n,
        'cloudflare.kv.response.returned_rows': 1n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'fail-client',
        'cloudflare.kv.query.keys.count': 1n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'fail-server',
        'cloudflare.kv.query.keys.count': 1n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'get-json',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.metadata': true,
        'cloudflare.kv.response.size': 20n,
        'cloudflare.kv.response.returned_rows': 1n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'get-json',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.query.type': 'json',
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.metadata': true,
        'cloudflare.kv.response.size': 20n,
        'cloudflare.kv.response.returned_rows': 1n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'success',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.query.type': 'stream',
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.metadata': true,
        'cloudflare.kv.response.size': 13n,
        'cloudflare.kv.response.returned_rows': 1n,
        closed: true,
      },
      {
        name: 'kv_get',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'get',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'success',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.query.type': 'arrayBuffer',
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.metadata': true,
        'cloudflare.kv.response.size': 13n,
        'cloudflare.kv.response.returned_rows': 1n,
        closed: true,
      },
      {
        name: 'kv_list',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'list',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.prefix': 'te',
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.size': 291n,
        'cloudflare.kv.response.list_complete': false,
        'cloudflare.kv.response.cursor': '6Ck1la0VxJ0djhidm1MdX2FyD',
        'cloudflare.kv.response.expiration': 1234n,
        'cloudflare.kv.response.returned_rows': 3n,
        closed: true,
      },
      {
        name: 'kv_list',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'list',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.limit': 100n,
        'cloudflare.kv.query.prefix': 'te',
        'cloudflare.kv.query.cursor': '123',
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.size': 6939n,
        'cloudflare.kv.response.list_complete': true,
        'cloudflare.kv.response.expiration': 1234n,
        'cloudflare.kv.response.returned_rows': 100n,
        closed: true,
      },
      {
        name: 'kv_list',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'list',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.prefix': 'not-found',
        'cloudflare.kv.response.cache_status': 'HIT',
        'cloudflare.kv.response.size': 32n,
        'cloudflare.kv.response.list_complete': true,
        'cloudflare.kv.response.returned_rows': 0n,
        closed: true,
      },
      {
        name: 'kv_put',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'put',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'foo_with_exp',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.query.expiration': 10n,
        'cloudflare.kv.query.value_type': 'text',
        'cloudflare.kv.query.payload.size': 4n,
        closed: true,
      },
      {
        name: 'kv_put',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'put',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'foo_with_expTtl',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.query.expiration_ttl': 15n,
        'cloudflare.kv.query.value_type': 'text',
        'cloudflare.kv.query.payload.size': 23n,
        closed: true,
      },
      {
        name: 'kv_put',
        'db.system.name': 'cloudflare-kv',
        'db.operation.name': 'put',
        'cloudflare.binding.name': 'KV',
        'cloudflare.binding.type': 'KV',
        'cloudflare.kv.query.keys': 'foo_with_expTtl',
        'cloudflare.kv.query.keys.count': 1n,
        'cloudflare.kv.query.expiration_ttl': 15n,
        'cloudflare.kv.query.value_type': 'ArrayBuffer',
        'cloudflare.kv.query.payload.size': 256n,
        closed: true,
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
        assert.deepStrictEqual(received[i], expected[i]);
      } catch (e) {
        console.error(`value: ${i} does not match`);
        console.log(e);
        errors.push(e);
      }
    }
    if (errors.length > 0) {
      throw 'kv spans are incorrect';
    }
  },
};
