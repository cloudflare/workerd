// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker that validates both directions of JSRPC callback tracing.
// The CallbackService invocation contains its dispatch and outbound stub calls.

// The caller invocation contains the outbound service call and transient re-entry dispatches.
// Each callback span must nest under the jsRpcCall that originated it.

import * as assert from 'node:assert';
import unsafe from 'workerd:unsafe';

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
      complete: false,
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
      } else if (type === 'outcome') {
        data.complete = true;
      }
    };
  },
};

function jsRpcCalls(data) {
  if (!data) return [];
  return [...data.spans.entries()]
    .filter(([, span]) => span.name === 'jsRpcCall')
    .map(([spanId, span]) => ({ spanId, ...span }));
}

// Find both callback invocations and their spans, or return incomplete results.
// Tail events are delivered asynchronously, so callers poll.
function findCallbackSpans() {
  let caller = null;
  let callee = null;
  for (const data of invocations.values()) {
    const calls = jsRpcCalls(data);
    if (
      data.onset.info === 'jsrpc' &&
      data.onset.entrypoint === 'CallbackService'
    ) {
      callee = data;
    } else if (
      calls.some(
        (span) =>
          span.attrs['jsrpc.method'] === 'invokeCallbacks' &&
          span.attrs['jsrpc.target_kind'] === 'fetcher'
      )
    ) {
      caller = data;
    }
  }

  const callerCalls = jsRpcCalls(caller);
  const calleeCalls = jsRpcCalls(callee);
  return {
    caller,
    callee,
    callerDispatch: callerCalls.find(
      (span) => span.attrs['jsrpc.method'] === 'invokeCallbacks'
    ),
    calleeDispatch: calleeCalls.find(
      (span) => span.attrs['jsrpc.method'] === 'invokeCallbacks'
    ),
    callerTransientCalls: callerCalls.filter(
      (span) => span.attrs['jsrpc.target_kind'] === 'transient'
    ),
    calleeStubCalls: calleeCalls.filter(
      (span) => span.attrs['jsrpc.target_kind'] === 'stub'
    ),
  };
}

export const test = {
  async test() {
    const tracingEnabled = unsafe.isTestAutogateEnabled();
    // Poll until the expected spans arrive or, with tracing disabled, the invocation completes.
    // Tail events are asynchronous, so avoid relying on a fixed delay.
    const deadline = Date.now() + 5000;
    let found = findCallbackSpans();
    while (
      !(tracingEnabled
        ? found.callerDispatch &&
          found.calleeDispatch &&
          found.callerTransientCalls.length === 3 &&
          found.calleeStubCalls.length === 3
        : found.callee?.complete) &&
      Date.now() < deadline
    ) {
      await scheduler.wait(10);
      found = findCallbackSpans();
    }

    const {
      caller,
      callee,
      callerDispatch,
      calleeDispatch,
      callerTransientCalls,
      calleeStubCalls,
    } = found;
    assert.ok(
      callee,
      'Could not find the CallbackService JSRPC invocation in tail events'
    );
    if (!tracingEnabled) {
      assert.ok(callee.complete, 'CallbackService invocation did not complete');
      assert.strictEqual(
        jsRpcCalls(callee).length,
        0,
        'jsRpcCall spans must not be emitted while the autogate is disabled'
      );
      return;
    }

    assert.ok(caller, 'Could not find the caller invocation in tail events');
    assert.ok(callerDispatch, 'Missing caller jsRpcCall for invokeCallbacks');
    assert.ok(calleeDispatch, 'Missing callee jsRpcCall for invokeCallbacks');
    assert.strictEqual(
      calleeStubCalls.length,
      3,
      'Expected three outbound stub callback spans in the callee'
    );
    assert.strictEqual(
      callerTransientCalls.length,
      3,
      'Expected three transient callback dispatch spans in the caller'
    );

    const expectedMethods = new Set(['(this)', 'invokeTarget', 'invokeProxy']);
    assert.deepStrictEqual(
      new Set(calleeStubCalls.map((span) => span.attrs['jsrpc.method'])),
      expectedMethods,
      'Expected one callee stub span for each transient argument call'
    );
    assert.deepStrictEqual(
      new Set(callerTransientCalls.map((span) => span.attrs['jsrpc.method'])),
      expectedMethods,
      'Expected one caller transient span for each callback dispatch'
    );

    // Callee stub calls must stay under the server dispatch after the async boundary.
    // This verifies the async trace scopes installed by JsRpcTargetBase::callImpl().
    for (const callbackSpan of calleeStubCalls) {
      assert.strictEqual(
        callbackSpan.parentId,
        calleeDispatch.spanId,
        `Callee callback ${callbackSpan.attrs['jsrpc.method']} should nest under invokeCallbacks`
      );
      assert.notStrictEqual(
        callbackSpan.parentId,
        callee.rootSpanId,
        `Callee callback ${callbackSpan.attrs['jsrpc.method']} must not nest under the onset`
      );
    }

    // Caller transient dispatches must stay under the call that exported their capabilities.
    // This verifies the TransientJsRpcTarget::originatingCall path.
    for (const callbackSpan of callerTransientCalls) {
      assert.strictEqual(
        callbackSpan.parentId,
        callerDispatch.spanId,
        `Caller callback ${callbackSpan.attrs['jsrpc.method']} should nest under invokeCallbacks`
      );
      assert.notStrictEqual(
        callbackSpan.parentId,
        caller.rootSpanId,
        `Caller callback ${callbackSpan.attrs['jsrpc.method']} must not nest under the onset`
      );
    }
  },
};
