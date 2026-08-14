// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker asserting that a callee's per-call jsRpcCall span is attributed to the
// caller's per-call jsRpcCall span rather than only to the session.
//
// Two invocations take part:
//
//   caller (the test() handler)
//     jsRpcSession                      <- covers the whole session
//       jsRpcCall  (getCounter)         <- opens the session
//         jsRpcCall  (increment)        <- reuses the session
//
//   callee (CounterService, jsrpc onset)
//     onset parent                       <- caller's getCounter jsRpcCall
//     jsRpcCall  (getCounter dispatch)  <- jsrpc.caller_span_id = caller's getCounter jsRpcCall
//     jsRpcCall  (increment dispatch)   <- jsrpc.caller_span_id = caller's increment jsRpcCall
//
// Both callee spans belong to a single invocation (one onset), so the onset's parent alone
// can identify only the call that opened the session. Each dispatch therefore also carries a link
// to its specific caller call while remaining a child of its own invocation root.

import * as assert from 'node:assert';

// invocationId -> { onset, rootSpanId, spans: Map(spanId -> {name, parentId, attrs}) }
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
    .filter(([, s]) => s.name === name)
    .map(([spanId, s]) => ({ spanId, ...s }));
}

// Returns the caller and callee invocations once both have reported the spans we need, else nulls.
// Tail events arrive asynchronously, so callers poll.
function findInvocations() {
  let callee = null;
  let caller = null;
  for (const data of invocations.values()) {
    if (
      data.onset.info === 'jsrpc' &&
      data.onset.entrypoint === 'CounterService'
    ) {
      callee = data;
    } else if (spansNamed(data, 'jsRpcSession').length > 0) {
      caller = data;
    }
  }
  if (!caller || !callee) return { caller: null, callee: null };

  // Expect both dispatches on the callee and both client-side calls on the caller.
  if (spansNamed(callee, 'jsRpcCall').length < 2)
    return { caller: null, callee: null };
  if (spansNamed(caller, 'jsRpcCall').length < 2)
    return { caller: null, callee: null };
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

    const callerCalls = spansNamed(caller, 'jsRpcCall');
    const calleeCalls = spansNamed(callee, 'jsRpcCall');
    const callerCallIds = new Set(callerCalls.map((s) => s.spanId));
    const callerCallsByMethod = new Map(
      callerCalls.map((span) => [span.attrs['jsrpc.method'], span])
    );

    const sessionSpans = spansNamed(caller, 'jsRpcSession');
    assert.strictEqual(
      sessionSpans.length,
      1,
      'Expected exactly one jsRpcSession span'
    );
    const sessionSpanId = sessionSpans[0].spanId;

    // Sanity check that the caller nests its own calls as expected: the increment call was made on
    // a stub returned by getCounter, so it nests under getCounter's span.
    const callerGetCounter = callerCalls.find(
      (s) => s.attrs['jsrpc.method'] === 'getCounter'
    );
    assert.ok(callerGetCounter, "Missing caller's getCounter jsRpcCall span");
    assert.strictEqual(
      callerGetCounter.parentId,
      sessionSpanId,
      "The caller's first call should nest under the jsRpcSession span"
    );
    assert.strictEqual(
      callee.onset.parentId,
      callerGetCounter.spanId,
      'The callee onset should be parented to the jsRpcCall that opened the session'
    );

    // The core assertions: every callee dispatch span is linked to a specific caller call, while
    // remaining contained within its own invocation.
    for (const span of calleeCalls) {
      const method = span.attrs['jsrpc.method'];
      const link = span.attrs['jsrpc.caller_span_id'];

      assert.ok(
        link,
        `Callee jsRpcCall (${method}) should carry a jsrpc.caller_span_id attribute`
      );
      assert.notStrictEqual(
        link,
        sessionSpanId,
        `Callee jsRpcCall (${method}) should link to a specific call, not the session span`
      );
      assert.ok(
        callerCallIds.has(link),
        `Callee jsRpcCall (${method}) should link to one of the caller's jsRpcCall spans ` +
          `(jsrpc.caller_span_id=${link}, caller call spans=${[...callerCallIds].join(',')})`
      );
      assert.strictEqual(
        link,
        callerCallsByMethod.get(method)?.spanId,
        `Callee jsRpcCall (${method}) should link to the caller's matching method span`
      );

      // The span itself must stay within its own invocation so this tail stream is self-contained.
      assert.ok(
        span.parentId === callee.rootSpanId || callee.spans.has(span.parentId),
        `Callee jsRpcCall (${method}) must be parented within its own invocation ` +
          `(parentId=${span.parentId})`
      );
    }

    // The two dispatches must link to *different* caller calls, proving the link tracks the
    // individual call rather than something session-wide.
    const distinctLinks = new Set(
      calleeCalls.map((s) => s.attrs['jsrpc.caller_span_id'])
    );
    assert.strictEqual(
      distinctLinks.size,
      calleeCalls.length,
      'Each callee jsRpcCall should link to a distinct caller call'
    );
  },
};
