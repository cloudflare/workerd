// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tail-worker side of tracing-log-attribution-test. Collects streaming tail events
// and asserts that Log/Exception/DiagnosticChannelEvent events carry the spanId of
// the currently-active user span (as opened by `withSpan`/`enterSpan`), rather
// than the root invocation's spanId.

import assert from 'node:assert';

// Per-invocation collector:
//   spans: Map<spanId, { spanId, parentSpanId, name, invocationId, closed, ...attrs }>
//   logs:  Array<{ message, spanContextSpanId, invocationId }>
//   exceptions: Array<{ name, message, spanContextSpanId, invocationId }>
//   diagnosticChannelEvents: Array<{ channel, spanContextSpanId, invocationId }>
//   topLevelSpanId: string  // from the onset event
const state = {
  invocationPromises: [],
  byInvocation: new Map(),
};

function getOrCreateInvocation(invocationId) {
  let inv = state.byInvocation.get(invocationId);
  if (!inv) {
    inv = {
      invocationId,
      spans: new Map(),
      logs: [],
      exceptions: [],
      diagnosticChannelEvents: [],
      topLevelSpanId: null,
    };
    state.byInvocation.set(invocationId, inv);
  }
  return inv;
}

export default {
  tailStream(event, env, ctx) {
    // `event` is the onset event. `event.event.spanId` is the top-level span ID
    // for this invocation; every other event's `spanContext.spanId` describes the
    // span that was active at event time.
    const inv = getOrCreateInvocation(event.invocationId);
    inv.topLevelSpanId = event.event.spanId;

    let resolveFn;
    state.invocationPromises.push(
      new Promise((resolve) => {
        resolveFn = resolve;
      })
    );

    return (event) => {
      const inv = getOrCreateInvocation(event.invocationId);
      const currentSpanId = event.spanContext?.spanId;
      switch (event.event.type) {
        case 'spanOpen':
          inv.spans.set(event.event.spanId, {
            spanId: event.event.spanId,
            parentSpanId: currentSpanId,
            name: event.event.name,
            invocationId: event.invocationId,
          });
          break;
        case 'attributes': {
          // Attributes belong to the span whose id equals spanContext.spanId.
          const span = inv.spans.get(currentSpanId);
          if (span) {
            for (const { name, value } of event.event.info) {
              span[name] = value;
            }
          }
          break;
        }
        case 'spanClose': {
          const span = inv.spans.get(currentSpanId);
          if (span) span.closed = true;
          break;
        }
        case 'log':
          inv.logs.push({
            message: event.event.message,
            spanContextSpanId: currentSpanId,
            invocationId: event.invocationId,
          });
          break;
        case 'exception':
          inv.exceptions.push({
            name: event.event.name,
            message: event.event.message,
            spanContextSpanId: currentSpanId,
            invocationId: event.invocationId,
          });
          break;
        case 'diagnosticChannel':
          inv.diagnosticChannelEvents.push({
            channel: event.event.channel,
            spanContextSpanId: currentSpanId,
            invocationId: event.invocationId,
          });
          break;
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};

// Locate the invocation whose span set contains a span with the given name.
// Also returns that span for convenience.
function findInvocationBySpanName(spanName) {
  const matches = [];
  for (const inv of state.byInvocation.values()) {
    for (const span of inv.spans.values()) {
      if (span.name === spanName) {
        matches.push({ inv, span });
      }
    }
  }
  assert.strictEqual(
    matches.length,
    1,
    `Expected exactly one invocation containing span "${spanName}", got ${matches.length}`
  );
  return matches[0];
}

// Locate the invocation that produced exactly the set of top-level (non-nested)
// spans in `spanNames`. Used for the logOutsideAnySpan case, which opens no
// user spans but is identified by the single log it emits.
// Log events have their `message` set to the JSON-parsed console.log argument
// array (see Log::ToJs in trace-stream.c++), so we need a deep stringify to
// search within.
function logContains(log, needle) {
  try {
    return JSON.stringify(log.message).includes(needle);
  } catch (_) {
    return false;
  }
}

function findInvocationByLogMessage(message) {
  const matches = [];
  for (const inv of state.byInvocation.values()) {
    if (inv.logs.some((l) => logContains(l, message))) {
      matches.push(inv);
    }
  }
  assert.strictEqual(
    matches.length,
    1,
    `Expected exactly one invocation with log message containing "${message}", got ${matches.length}`
  );
  return matches[0];
}

function findLog(inv, substring) {
  const matches = inv.logs.filter((l) => logContains(l, substring));
  assert.strictEqual(
    matches.length,
    1,
    `Expected exactly one log containing "${substring}" in invocation ${inv.invocationId}, got ${matches.length}`
  );
  return matches[0];
}

export const validate = {
  async test() {
    // Wait for every invocation's `outcome` event to arrive.
    await Promise.allSettled(state.invocationPromises);

    // ---------- Case 1: logInsideEnterSpan ----------
    // Log inside a single enterSpan must be attributed to that span (not the root).
    {
      const { inv, span } = findInvocationBySpanName('log-attr-single');
      const log = findLog(inv, 'log-attr-single-message');
      assert.strictEqual(
        log.spanContextSpanId,
        span.spanId,
        `logInsideEnterSpan: log.spanContext.spanId (${log.spanContextSpanId}) should equal the enclosing span id (${span.spanId})`
      );
      assert.notStrictEqual(
        log.spanContextSpanId,
        inv.topLevelSpanId,
        'logInsideEnterSpan: log should NOT be attributed to the root invocation span'
      );
    }

    // ---------- Case 2: logInsideNestedEnterSpan ----------
    {
      const { inv, span: outer } = findInvocationBySpanName('log-attr-outer');
      const inner = Array.from(inv.spans.values()).find(
        (s) => s.name === 'log-attr-inner'
      );
      assert.ok(inner, 'expected an inner span');
      assert.strictEqual(
        inner.parentSpanId,
        outer.spanId,
        'inner span must be a child of outer'
      );

      const beforeInner = findLog(inv, 'log-attr-outer-before-inner');
      const innerMsg = findLog(inv, 'log-attr-inner-message');
      const afterInner = findLog(inv, 'log-attr-outer-after-inner');

      assert.strictEqual(
        beforeInner.spanContextSpanId,
        outer.spanId,
        'log before inner must be attributed to outer'
      );
      assert.strictEqual(
        innerMsg.spanContextSpanId,
        inner.spanId,
        'log inside inner must be attributed to inner (not outer, not root)'
      );
      assert.notStrictEqual(
        innerMsg.spanContextSpanId,
        inv.topLevelSpanId,
        'log inside inner must NOT be attributed to the root'
      );
      assert.strictEqual(
        afterInner.spanContextSpanId,
        outer.spanId,
        'log after inner closes must be re-attributed to outer'
      );
    }

    // ---------- Case 3: logAcrossAwait ----------
    // This is the acid test: the fix only works if the AsyncContextFrame carrying
    // the user span propagates across the microtask boundary introduced by `await`.
    {
      const { inv, span } = findInvocationBySpanName('log-attr-async');
      const log = findLog(inv, 'log-attr-async-message');
      assert.strictEqual(
        log.spanContextSpanId,
        span.spanId,
        'logAcrossAwait: log inside async enterSpan after await must be attributed to that span'
      );
      assert.notStrictEqual(
        log.spanContextSpanId,
        inv.topLevelSpanId,
        'logAcrossAwait: log must NOT be attributed to the root'
      );
    }

    // ---------- Case 4: logOutsideAnySpan (fall-back path) ----------
    // No user span is active, so the log's spanContext.spanId must equal the
    // invocation's top-level (onset) span id. This proves we don't break logs
    // that have no user span to attribute to.
    {
      const inv = findInvocationByLogMessage('log-attr-root-message');
      const log = findLog(inv, 'log-attr-root-message');
      assert.strictEqual(
        log.spanContextSpanId,
        inv.topLevelSpanId,
        'logOutsideAnySpan: with no user span active, log must fall back to the root invocation span'
      );
    }

    // ---------- Case 5: diagnosticsChannelInsideEnterSpan ----------
    {
      const { inv, span } = findInvocationBySpanName('log-attr-dc');
      assert.strictEqual(
        inv.diagnosticChannelEvents.length,
        1,
        'expected exactly one diagnosticChannelEvent'
      );
      const dce = inv.diagnosticChannelEvents[0];
      assert.strictEqual(
        dce.channel,
        'log-attr-dc-channel',
        'diagnosticChannelEvent channel name'
      );
      assert.strictEqual(
        dce.spanContextSpanId,
        span.spanId,
        'diagnosticChannelEvent must be attributed to the enclosing user span'
      );
      assert.notStrictEqual(
        dce.spanContextSpanId,
        inv.topLevelSpanId,
        'diagnosticChannelEvent must NOT be attributed to the root'
      );
    }

    console.log('All tracing-log-attribution tests passed!');
  },
};
