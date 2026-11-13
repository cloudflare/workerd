// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  createHierarchyAwareCollector,
  findSpanByName,
} from 'instrumentation-test-helper';

const collector = createHierarchyAwareCollector();
export default { tailStream: collector.tailStream };

// Keep in sync with tracing-active-span-context-test.js.
const REMOTE_CONTEXT = {
  traceId: '4bf92f3577b34da6a3ce929d0e0e4736',
  spanId: '00f067aa0ba902b7',
};
const SAME_TRACE_SPAN_ID = '1234567890abcdef';

// After the scenarios in tracing-active-span-context-test.js have run, validate the traces and
// parents encoded in the streaming-tail events. Spans are compared by tail session (one per
// invocation) because invocation IDs can repeat in tests.
export const validateHierarchy = {
  async test() {
    await collector.waitForCompletion();
    const { state } = collector;

    const span = (name) => findSpanByName(state, name);
    const fetchSpanUnder = (parent) =>
      findSpanByName(
        state,
        'fetch',
        (f) => f.session === parent.session && f.parentSpanId === parent.spanId
      );
    const invocationTraceId = (s) => state.sessions[s.session].traceId;
    // Checks that `s` was opened and closed in `traceId`.
    const assertTrace = (s, traceId) => {
      assert.strictEqual(s.traceId, traceId, `trace of "${s.name}"`);
      assert.ok(s.closed, `"${s.name}" should be closed`);
      assert.strictEqual(s.closeTraceId, traceId, `close trace of "${s.name}"`);
    };
    const assertParent = (child, parent) => {
      assert.strictEqual(
        child.parentSpanId,
        parent.spanId,
        `expected "${child.name}" to be a child of "${parent.name}"`
      );
      assert.strictEqual(child.session, parent.session);
      assertTrace(child, parent.traceId);
    };
    const assertRemoteChild = (s) => {
      assert.strictEqual(s.parentSpanId, REMOTE_CONTEXT.spanId);
      assertTrace(s, REMOTE_CONTEXT.traceId);
    };
    const assertTopLevel = (s) => {
      assert.strictEqual(
        s.parentSpanId,
        state.sessions[s.session].topLevelSpanId,
        `expected "${s.name}" to be a child of the invocation span`
      );
      assertTrace(s, invocationTraceId(s));
    };
    // Checks that `s` starts a new trace: it has no parent and a trace other than the invocation's.
    const assertRoot = (s) => {
      assert.strictEqual(
        s.parentSpanId,
        undefined,
        `expected "${s.name}" to have no parent`
      );
      assert.notStrictEqual(s.traceId, invocationTraceId(s));
      assertTrace(s, s.traceId);
    };
    // Checks that the log with `message` was reported in `traceId`, under `spanId`.
    const assertLog = (message, traceId, spanId) => {
      const logs = state.logs.filter((log) => log.message[0] === message);
      assert.strictEqual(logs.length, 1, `logs for "${message}"`);
      assert.strictEqual(logs[0].traceId, traceId, `trace of "${message}"`);
      assert.strictEqual(logs[0].spanId, spanId, `span of "${message}"`);
    };
    // Checks that the service that `fetchSpan` called at `url` was invoked as a child of the fetch
    // span, in its trace.
    const assertCalleeJoinsTrace = (fetchSpan, url) => {
      const callees = state.sessions.filter((session) => session.url === url);
      assert.strictEqual(callees.length, 1, `invocations for ${url}`);
      assert.strictEqual(callees[0].traceId, fetchSpan.traceId);
      assert.strictEqual(callees[0].parentSpanId, fetchSpan.spanId);
    };

    // remoteContextParentsNewSpans: new spans, built-in instrumentation and logs are in the
    // remote trace, under the remote span, and the fetched service joins that trace.
    assertRemoteChild(span('wasc-remote-start-span'));
    const enterSpan = span('wasc-remote-enter-span');
    assertRemoteChild(enterSpan);
    assertParent(span('wasc-remote-grandchild'), enterSpan);
    const remoteFetch = fetchSpanUnder(enterSpan);
    assertParent(remoteFetch, enterSpan);
    assertCalleeJoinsTrace(remoteFetch, 'http://mock/wasc-remote');
    assertLog('wasc-remote-log', REMOTE_CONTEXT.traceId, enterSpan.spanId);

    // remoteContextLogsAndAwait: a log directly in the callback is under the remote span, and the
    // remote span stays active after an await.
    assertLog(
      'wasc-remote-direct-log',
      REMOTE_CONTEXT.traceId,
      REMOTE_CONTEXT.spanId
    );
    assertRemoteChild(span('wasc-remote-after-await'));

    // unsampledRemoteContextIsRecorded: spans under an unsampled remote span are still recorded.
    assertRemoteChild(span('wasc-unsampled-child'));

    // sameTraceContext: a context in the invocation's trace parents spans in that trace.
    const sameTraceChild = span('wasc-same-trace-child');
    assert.strictEqual(sameTraceChild.parentSpanId, SAME_TRACE_SPAN_ID);
    assertTrace(sameTraceChild, invocationTraceId(sameTraceChild));

    // localSpanInsideRemoteContext: an activated local span takes precedence, and activating the
    // remote span again parents spans under it.
    const localParent = span('wasc-local-parent');
    assertTopLevel(localParent);
    assertParent(span('wasc-local-child'), localParent);
    assertRemoteChild(span('wasc-reactivated-remote-child'));

    // undefinedContextStartsNewTraces: each span with no parent starts its own trace, as in
    // OpenTelemetry, and the enclosing span is restored afterwards.
    const enclosing = span('wasc-root-enclosing');
    assertTopLevel(enclosing);
    const rootA = span('wasc-root-a');
    assertRoot(rootA);
    assertParent(span('wasc-root-a-child'), rootA);
    assertParent(fetchSpanUnder(rootA), rootA);
    assertLog('wasc-root-log', rootA.traceId, rootA.spanId);
    const rootB = span('wasc-root-b');
    assertRoot(rootB);
    assert.notStrictEqual(rootB.traceId, rootA.traceId);
    const rootFetch = findSpanByName(
      state,
      'fetch',
      (f) => f.session === rootA.session && f.parentSpanId === undefined
    );
    assertRoot(rootFetch);
    assert.notStrictEqual(rootFetch.traceId, rootA.traceId);
    assert.notStrictEqual(rootFetch.traceId, rootB.traceId);
    assertCalleeJoinsTrace(rootFetch, 'http://mock/wasc-root-fetch');
    assertParent(span('wasc-root-after'), enclosing);

    // invalidContextStartsNewTraces: an invalid context is treated as no parent, including when
    // its span is activated again.
    assertTopLevel(span('wasc-invalid-enclosing'));
    assertRoot(span('wasc-invalid-root'));
    assertRoot(span('wasc-invalid-reactivated-root'));

    // durableObjectOverlappingRequests: the first request's spans stay in its own invocation, even
    // though the second request is the Durable Object's current request when they are created.
    const firstMarker = span('wasc-do-first-marker');
    const secondMarker = span('wasc-do-second-marker');
    assert.notStrictEqual(firstMarker.session, secondMarker.session);
    const doRemoteChild = span('wasc-do-remote-child');
    assertRemoteChild(doRemoteChild);
    assert.strictEqual(doRemoteChild.session, firstMarker.session);
    const doRoot = span('wasc-do-root');
    assertRoot(doRoot);
    assert.strictEqual(doRoot.session, firstMarker.session);
  },
};
