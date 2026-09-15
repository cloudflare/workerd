// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker asserting that JSRPC traces model logical calls rather than sessions.
//
//   caller
//     jsRpcCall  (getCounter)
//       jsRpcCall  (pipelined increment)
//     jsRpcCall  (incrementDuplicate after getCounter resolves)
//
//   callee
//     jsRpcCall  (getCounter)          <- caller getCounter
//     jsRpcCall  (increment)           <- caller increment
//     jsRpcCall  (incrementDuplicate)  <- caller incrementDuplicate

import * as assert from 'node:assert';

let invocations = new Map();

export default {
  tailStream(onsetEvent, env, ctx) {
    const data = {
      onset: {
        info: onsetEvent.event.info?.type,
        entrypoint: onsetEvent.event.entrypoint,
        parentId: onsetEvent.spanContext.spanId,
      },
      rootSpanId: onsetEvent.event.spanId,
      spans: new Map(),
    };
    invocations.set(onsetEvent.invocationId, data);

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

function spansNamed(data, name) {
  return [...data.spans.entries()]
    .filter(([, span]) => span.name === name)
    .map(([spanId, span]) => ({ spanId, ...span }));
}

function findInvocations() {
  let callee = null;
  let caller = null;
  for (const data of invocations.values()) {
    const calls = spansNamed(data, 'jsRpcCall');
    if (
      data.onset.info === 'jsrpc' &&
      data.onset.entrypoint === 'CounterService'
    ) {
      callee = data;
    } else if (
      calls.some(
        (span) =>
          span.attrs['jsrpc.method'] === 'getCounter' &&
          span.attrs['jsrpc.target_kind'] === 'fetcher'
      )
    ) {
      caller = data;
    }
  }
  if (!caller || !callee) return { caller: null, callee: null };

  const expectedMethods = new Set([
    'getCounter',
    'increment',
    'incrementDuplicate',
  ]);
  const callerCalls = spansNamed(caller, 'jsRpcCall');
  const calleeCalls = spansNamed(callee, 'jsRpcCall');
  const hasExpectedMethods = (calls) => {
    const methods = new Set(calls.map((span) => span.attrs['jsrpc.method']));
    return (
      calls.length === expectedMethods.size &&
      methods.size === expectedMethods.size &&
      [...expectedMethods].every((method) => methods.has(method))
    );
  };
  if (!hasExpectedMethods(callerCalls) || !hasExpectedMethods(calleeCalls)) {
    return { caller: null, callee: null };
  }
  return { caller, callee };
}

export const test = {
  async test() {
    const deadline = Date.now() + 5000;
    let found = findInvocations();
    while (!found.callee && Date.now() < deadline) {
      await scheduler.wait(10);
      found = findInvocations();
    }

    const { caller, callee } = found;
    assert.ok(caller, 'Could not find the caller invocation in tail events');
    assert.ok(
      callee,
      'Could not find the CounterService JSRPC invocation in tail events'
    );

    assert.strictEqual(
      spansNamed(caller, 'jsRpcSession').length,
      0,
      'JSRPC sessions must not be emitted as user spans'
    );

    const callerCallsByMethod = new Map(
      spansNamed(caller, 'jsRpcCall').map((span) => [
        span.attrs['jsrpc.method'],
        span,
      ])
    );
    const calleeCalls = spansNamed(callee, 'jsRpcCall');
    const callerGetCounter = callerCallsByMethod.get('getCounter');
    const callerIncrement = callerCallsByMethod.get('increment');
    const callerIncrementDuplicate =
      callerCallsByMethod.get('incrementDuplicate');

    assert.strictEqual(
      callerGetCounter.parentId,
      caller.rootSpanId,
      'The first call should be a child of the caller operation'
    );
    assert.strictEqual(
      callerIncrement.parentId,
      callerGetCounter.spanId,
      'A pipelined call should be a child of the call whose promise it uses'
    );
    assert.strictEqual(
      callerIncrementDuplicate.parentId,
      caller.rootSpanId,
      'A call on a resolved stub should be a new operation under the current context'
    );

    for (const serverSpan of calleeCalls) {
      const method = serverSpan.attrs['jsrpc.method'];
      const clientSpan = callerCallsByMethod.get(method);
      assert.ok(clientSpan, `Missing caller span for ${method}`);
      assert.strictEqual(
        serverSpan.parentId,
        clientSpan.spanId,
        `The ${method} server span should be a child of its matching client span`
      );
      assert.strictEqual(
        serverSpan.attrs['jsrpc.caller_span_id'],
        clientSpan.spanId,
        `The ${method} caller attribute should match its parent`
      );
    }
  },
};
