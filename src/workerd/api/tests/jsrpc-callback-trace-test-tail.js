// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker that validates jsRpcCall span nesting for callback arguments.
//
// Within the CallbackService (jsrpc) invocation we expect:
//   onset
//     jsRpcCall (jsrpc.method = "invokeCallback", target_kind = "entrypoint")  <- server dispatch
//       jsRpcCall (target_kind = "stub")                                       <- server invokes cb
//
// The inner (callback) jsRpcCall must be a child of the method's jsRpcCall span, proving the
// argument stub recorded the correct originating call.

import * as assert from 'node:assert';

// Per-invocation span data: invocationId -> { onset, rootSpanId, spans: Map(spanId -> span) }.
// Each span records its name, its parent span ID (from the spanOpen's spanContext), and any
// attributes merged from subsequent attributes events targeting that span.
let invocations = new Map();

export default {
  tailStream(onsetEvent, env, ctx) {
    const invocationId = onsetEvent.invocationId;
    const rootSpanId = onsetEvent.event.spanId;
    const data = {
      onset: {
        info: onsetEvent.event.info?.type,
        entrypoint: onsetEvent.event.entrypoint,
      },
      rootSpanId,
      spans: new Map(),
    };
    invocations.set(invocationId, data);

    return (event) => {
      const type = event.event.type;
      if (type === 'spanOpen') {
        data.spans.set(event.event.spanId, {
          name: event.event.name,
          parentId: event.spanContext.spanId,
          attrs: {},
        });
      } else if (type === 'attributes') {
        const span = data.spans.get(event.spanContext.spanId);
        if (span) {
          for (const { name, value } of event.event.info) {
            span.attrs[name] = value;
          }
        }
      }
    };
  },
};

// Find the server-side CallbackService JSRPC invocation and its two jsRpcCall spans, or return
// null if they haven't all arrived yet. Tail events are delivered asynchronously, so callers poll.
function findCallbackSpans() {
  let target = null;
  for (const data of invocations.values()) {
    if (
      data.onset.info === 'jsrpc' &&
      data.onset.entrypoint === 'CallbackService'
    ) {
      target = data;
      break;
    }
  }
  if (!target) return { target: null, methodSpan: null, callbackSpan: null };

  const jsRpcCalls = [...target.spans.entries()]
    .filter(([, s]) => s.name === 'jsRpcCall')
    .map(([spanId, s]) => ({ spanId, ...s }));

  // The method dispatch span (server-side jsRpcCall for invokeCallback) and the callback
  // invocation span (the server calling back into the client stub, target_kind=stub).
  const methodSpan = jsRpcCalls.find(
    (s) => s.attrs['jsrpc.method'] === 'invokeCallback'
  );
  const callbackSpan = jsRpcCalls.find(
    (s) => s.attrs['jsrpc.target_kind'] === 'stub'
  );
  return { target, methodSpan, callbackSpan };
}

export const test = {
  async test() {
    // Poll until the invocation and both spans have arrived rather than relying on a fixed delay.
    const deadline = Date.now() + 5000;
    let found = findCallbackSpans();
    while (!(found.methodSpan && found.callbackSpan) && Date.now() < deadline) {
      await scheduler.wait(10);
      found = findCallbackSpans();
    }

    const { target, methodSpan, callbackSpan } = found;
    assert.ok(
      target,
      'Could not find the CallbackService JSRPC invocation in tail events'
    );
    assert.ok(methodSpan, 'Missing jsRpcCall span for invokeCallback');
    assert.ok(
      callbackSpan,
      'Missing jsRpcCall span for the callback invocation (target_kind=stub)'
    );

    // The core assertion: the callback jsRpcCall nests under the method jsRpcCall, not the onset.
    assert.strictEqual(
      callbackSpan.parentId,
      methodSpan.spanId,
      'Callback jsRpcCall should nest under the method jsRpcCall span, not the onset'
    );
    assert.notStrictEqual(
      callbackSpan.parentId,
      target.rootSpanId,
      'Callback jsRpcCall must not be parented directly under the onset span'
    );
  },
};
