// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';
import unsafe from 'workerd:unsafe';

// Flat array of all invocations observed by the tail handler.
// Each entry captures trace metadata and concatenated event JSON.
let allInvocations = [];

// JSON.stringify() does not support BigInt by default - manually add that here.
BigInt.prototype.toJSON = function () {
  return this.toString();
};

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tailStream(event, env, ctx) {
    // Onset event, must be singleton
    let spanIdSet = new Set();

    // For scheduled and alarm tests, override scheduledTime to make this test deterministic.
    if (
      event.event.info.type == 'scheduled' ||
      event.event.info.type == 'alarm'
    ) {
      event.event.info.scheduledTime = '1970-01-01T00:00:00.000Z';
    }

    const topLevelSpanId = event.event.spanId;
    // Set inner spanId to zero to have deterministic output, but save it first.
    event.event.spanId = '0000000000000000';

    const entry = {
      traceId: event.spanContext.traceId,
      parentSpanId: event.spanContext.spanId, // undefined for top-level, caller's spanId for subrequests
      spanId: topLevelSpanId, // this invocation's own root span
      // All span IDs owned by this invocation (root + children from spanOpen events).
      // Used by buildTree to match a child invocation's parentSpanId to this invocation.
      allSpanIds: new Set([topLevelSpanId]),
      events: JSON.stringify(event.event),
    };
    allInvocations.push(entry);

    return (event) => {
      // The spanContext always has a non-zero spanId available for events other than Onset.
      assert.notStrictEqual(event.spanContext.spanId, undefined);
      assert.notStrictEqual(event.spanContext.spanId, '0000000000000000');

      // For outcome events (and all other tail events except for nested spans) the reported spanId
      // must match the span opened by the Onset event.
      if (event.event.type == 'outcome') {
        assert.strictEqual(event.spanContext.spanId, topLevelSpanId);
      } else if (event.event.type == 'spanOpen') {
        // Every SpanOpen event must have another span or the Onset as its parent, as described by
        // the SpanContext spanId.
        assert.ok(
          event.spanContext.spanId == topLevelSpanId ||
            spanIdSet.has(event.spanContext.spanId)
        );
        // Add the newly opened span to the set of spanIds.
        spanIdSet.add(event.event.spanId);
        entry.allSpanIds.add(event.event.spanId);
      } else if (
        event.event.type == 'attributes' ||
        event.event.type == 'spanClose'
      ) {
        // Every SpanClose/attributes event must point to the spanId of a previously opened span
        // OR the top-level span created by the onset event.
        assert.ok(
          spanIdSet.has(event.spanContext.spanId) ||
            event.spanContext.spanId === topLevelSpanId
        );
      }

      entry.events += JSON.stringify(event.event);
    };
  },
};

// Build a tree from the flat invocations array.
// Each invocation tracks allSpanIds (its root span + all child spans from spanOpen events).
// A child invocation's parentSpanId is matched against allSpanIds from invocations in the
// same trace (same traceId) to find its parent. This handles both:
//   - Subrequests: parentSpanId is the caller's user span ID (sequential, from spanOpen)
//   - Hibernation: parentSpanId is the caller's invocation root span ID (from fromEntropy)
// Scoping by traceId avoids false matches from sequential span IDs that reset per invocation.
function buildTree(invocations) {
  // First pass: create nodes and group by traceId.
  const byTraceId = new Map();
  const nodes = [];
  for (const inv of invocations) {
    const node = { events: inv.events, children: [] };
    nodes.push({ inv, node });
    if (!byTraceId.has(inv.traceId)) {
      byTraceId.set(inv.traceId, []);
    }
    byTraceId.get(inv.traceId).push({ inv, node });
  }

  // Build per-trace span ID index: maps spanId -> node within the same trace.
  const traceSpanIndex = new Map(); // traceId -> Map(spanId -> node)
  for (const [traceId, group] of byTraceId) {
    const index = new Map();
    for (const { inv, node } of group) {
      for (const sid of inv.allSpanIds) {
        index.set(sid, node);
      }
    }
    traceSpanIndex.set(traceId, index);
  }

  // Second pass: link children to parents within the same trace.
  const roots = [];
  for (const { inv, node } of nodes) {
    if (inv.parentSpanId === undefined) {
      roots.push(node);
    } else {
      const index = traceSpanIndex.get(inv.traceId);
      const parent = index.get(inv.parentSpanId);
      if (parent && parent !== node) {
        parent.children.push(node);
      } else {
        roots.push(node);
      }
    }
  }

  // Sort recursively for deterministic comparison.
  function sortTree(node) {
    node.children.sort((a, b) => a.events.localeCompare(b.events));
    for (const child of node.children) sortTree(child);
  }
  for (const root of roots) sortTree(root);
  roots.sort((a, b) => a.events.localeCompare(b.events));

  return roots;
}

// Verify that invocations with a parentSpanId have a non-zero value.
function verifyTraceIds(invocations) {
  for (const inv of invocations) {
    if (inv.parentSpanId !== undefined) {
      // parentSpanId must be non-zero.
      assert.notStrictEqual(
        inv.parentSpanId,
        '0000000000000000',
        'parentSpanId must be non-zero'
      );
    }
  }
}

// Helper to create a tree node.
function n(events, children = []) {
  return { events, children };
}

// Event strings shared between both expected trees (sorted alphabetically within each group).
const E = {
  // actor-alarms-test.js
  alarm:
    '{"type":"onset","executionModel":"durableObject","spanId":"0000000000000000","entrypoint":"DurableObjectExample","scriptTags":[],"info":{"type":"alarm","scheduledTime":"1970-01-01T00:00:00.000Z"}}{"type":"spanOpen","name":"durable_object_storage_getAlarm","spanId":"0000000000000001"}{"type":"spanClose","outcome":"ok"}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  doFetch:
    '{"type":"onset","executionModel":"durableObject","spanId":"0000000000000000","entrypoint":"DurableObjectExample","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://foo/test","headers":[]}}{"type":"spanOpen","name":"durable_object_storage_setAlarm","spanId":"0000000000000001"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"durable_object_storage_getAlarm","spanId":"0000000000000002"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"durable_object_storage_getAlarm","spanId":"0000000000000003"}{"type":"spanClose","outcome":"ok"}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

  // websocket/hibernation
  wsUpgrade:
    '{"type":"onset","executionModel":"durableObject","spanId":"0000000000000000","entrypoint":"DurableObjectExample","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://example.com/","headers":[{"name":"upgrade","value":"websocket"}]}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  wsHibernation:
    '{"type":"onset","executionModel":"durableObject","spanId":"0000000000000000","entrypoint":"DurableObjectExample","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://example.com/hibernation","headers":[{"name":"upgrade","value":"websocket"}]}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  wsMessage:
    '{"type":"onset","executionModel":"durableObject","spanId":"0000000000000000","entrypoint":"DurableObjectExample","scriptTags":[],"info":{"type":"hibernatableWebSocket","info":{"type":"message"}}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  wsClose:
    '{"type":"onset","executionModel":"durableObject","spanId":"0000000000000000","entrypoint":"DurableObjectExample","scriptTags":[],"info":{"type":"hibernatableWebSocket","info":{"type":"close","code":1000,"wasClean":true}}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

  // jsrpc
  myActorJsrpc:
    '{"type":"onset","executionModel":"durableObject","spanId":"0000000000000000","entrypoint":"MyActor","scriptTags":[],"info":{"type":"jsrpc"}}{"type":"log","level":"log","message":["baz"]}{"type":"attributes","info":[{"name":"jsrpc.method","value":"functionProperty"}]}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  jsrpcNonFunction:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","entrypoint":"MyService","scriptTags":[],"info":{"type":"jsrpc"}}{"type":"attributes","info":[{"name":"jsrpc.method","value":"nonFunctionProperty"}]}{"type":"log","level":"log","message":["bar"]}{"type":"log","level":"log","message":["foo"]}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  jsrpcGetCounter:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","entrypoint":"MyService","scriptTags":[],"info":{"type":"jsrpc"}}{"type":"attributes","info":[{"name":"jsrpc.method","value":"getCounter"}]}{"type":"log","level":"log","message":["bar"]}{"type":"log","level":"log","message":["getCounter called"]}{"type":"return"}{"type":"log","level":"log","message":["increment called on transient"]}{"type":"log","level":"log","message":["getValue called on transient"]}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  jsrpcDoSubrequest:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"custom"}}{"type":"spanOpen","name":"jsRpcSession","spanId":"0000000000000001"}{"type":"spanOpen","name":"durable_object_subrequest","spanId":"0000000000000002"}{"type":"spanOpen","name":"jsRpcSession","spanId":"0000000000000003"}{"type":"attributes","info":[{"name":"objectId","value":"af6dd8b6678e07bac992dae1bbbb3f385af19ebae7e5ea8c66d6341b246d3328"}]}{"type":"spanClose","outcome":"ok"}{"type":"spanClose","outcome":"ok"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

  // cacheMode
  cacheMode:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","entrypoint":"cacheMode","scriptTags":[],"info":{"type":"custom"}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

  // connect
  connectHandler:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","entrypoint":"connectHandler","scriptTags":[],"info":{"type":"custom"}}{"type":"spanOpen","name":"connect","spanId":"0000000000000001"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  connectHandlerProxy:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","entrypoint":"connectHandlerProxy","scriptTags":[],"info":{"type":"custom"}}{"type":"spanOpen","name":"connect","spanId":"0000000000000001"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  localAddressViaServiceBinding:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","entrypoint":"localAddressViaServiceBinding","scriptTags":[],"info":{"type":"custom"}}{"type":"spanOpen","name":"connect","spanId":"0000000000000001"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  connectTarget:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"connect"}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

  // http-test.js: main test() handler with fetches + scheduled
  httpTest:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"custom"}}{"type":"spanOpen","name":"fetch","spanId":"0000000000000001"}{"type":"attributes","info":[{"name":"network.protocol.name","value":"http"},{"name":"network.protocol.version","value":"HTTP/1.1"},{"name":"http.request.method","value":"POST"},{"name":"url.full","value":"http://placeholder/body-length"},{"name":"http.request.body.size","value":"3"},{"name":"http.response.status_code","value":"200"},{"name":"http.response.body.size","value":"22"}]}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"fetch","spanId":"0000000000000002"}{"type":"attributes","info":[{"name":"network.protocol.name","value":"http"},{"name":"network.protocol.version","value":"HTTP/1.1"},{"name":"http.request.method","value":"POST"},{"name":"url.full","value":"http://placeholder/body-length"},{"name":"http.response.status_code","value":"200"},{"name":"http.response.body.size","value":"31"}]}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"fetch","spanId":"0000000000000003"}{"type":"spanOpen","name":"scheduled","spanId":"0000000000000004"}{"type":"attributes","info":[{"name":"network.protocol.name","value":"http"},{"name":"network.protocol.version","value":"HTTP/1.1"},{"name":"http.request.method","value":"GET"},{"name":"url.full","value":"http://placeholder/ray-id"},{"name":"http.response.status_code","value":"200"},{"name":"http.response.body.size","value":"0"},{"name":"cloudflare.ray_id","value":"test-ray-id-123"}]}{"type":"spanClose","outcome":"ok"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"scheduled","spanId":"0000000000000005"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

  // http-test subrequest handlers
  fetchBodyLength:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"http://placeholder/body-length","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchRayId:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://placeholder/ray-id","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchNotFound:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://placeholder/not-found","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":404}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchWebSocket:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://placeholder/web-socket","headers":[{"name":"upgrade","value":"websocket"}]}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchEmptyUrl:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":404}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  scheduledEmpty:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"scheduled","scheduledTime":"1970-01-01T00:00:00.000Z","cron":""}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  scheduledCron:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"scheduled","scheduledTime":"1970-01-01T00:00:00.000Z","cron":"* * * * 30"}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

  // queue-test.js
  queueTest:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"custom"}}{"type":"spanOpen","name":"queue_send","spanId":"0000000000000001"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"queue_send","spanId":"0000000000000002"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"queue_send","spanId":"0000000000000003"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"queue_send","spanId":"0000000000000004"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"queue_send","spanId":"0000000000000005"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"queue","spanId":"0000000000000006"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchMsgText:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/message","headers":[{"name":"content-type","value":"application/octet-stream"},{"name":"x-msg-delay-secs","value":"2"},{"name":"x-msg-fmt","value":"text"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchMsgBytes:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/message","headers":[{"name":"content-type","value":"application/octet-stream"},{"name":"x-msg-fmt","value":"bytes"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchMsgJson:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/message","headers":[{"name":"content-type","value":"application/octet-stream"},{"name":"x-msg-fmt","value":"json"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchMsgV8:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/message","headers":[{"name":"content-type","value":"application/octet-stream"},{"name":"x-msg-fmt","value":"v8"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  fetchBatch:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/batch","headers":[{"name":"cf-queue-batch-bytes","value":"31"},{"name":"cf-queue-batch-count","value":"4"},{"name":"cf-queue-largest-msg","value":"13"},{"name":"content-type","value":"application/json"},{"name":"x-msg-delay-secs","value":"2"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
  queueConsumer:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"queue","queueName":"test-queue","batchSize":5}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

  // buffered tail worker traces
  trace:
    '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"trace","traces":[""]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
};

// Expected tree without propagation — every invocation is a root with no children.
// This is the same set of events as the old flat expected array, just wrapped in tree nodes.
const expectedFlat = [
  n(E.alarm),
  n(E.wsUpgrade),
  n(E.wsHibernation),
  n(E.doFetch),
  n(E.wsClose),
  n(E.wsMessage),
  n(E.myActorJsrpc),
  n(E.cacheMode),
  n(E.connectHandler),
  n(E.connectHandlerProxy),
  n(E.localAddressViaServiceBinding),
  n(E.jsrpcGetCounter),
  n(E.jsrpcNonFunction),
  n(E.connectTarget),
  n(E.jsrpcDoSubrequest),
  n(E.httpTest),
  n(E.queueTest),
  n(E.fetchEmptyUrl),
  n(E.fetchNotFound),
  n(E.fetchRayId),
  n(E.fetchWebSocket),
  n(E.fetchBodyLength),
  n(E.fetchBodyLength),
  n(E.fetchBatch),
  n(E.fetchMsgText),
  n(E.fetchMsgBytes),
  n(E.fetchMsgJson),
  n(E.fetchMsgV8),
  n(E.queueConsumer),
  n(E.scheduledEmpty),
  n(E.scheduledCron),
  n(E.trace),
  n(E.trace),
].sort((a, b) => a.events.localeCompare(b.events));

// Expected tree with propagation — subrequest callees are children of their callers.
// DOs that are called via subrequests inherit the caller's traceId and become children.
// DOs triggered by system events (alarms, hibernation wakeups) remain standalone roots.
const expectedWithPropagation = [
  // actor-alarms-test: DO fetch and alarm are independent roots (own traceId)
  n(E.alarm),
  n(E.doFetch),

  // websocket/hibernation: independent roots
  n(E.wsUpgrade),
  n(E.wsHibernation),
  n(E.wsMessage),
  n(E.wsClose),

  // cacheMode: standalone
  n(E.cacheMode),

  // connect: handler, proxy, and service-binding test are separate top-level tests, target is
  // independent
  n(E.connectHandler),
  n(E.connectHandlerProxy),
  n(E.localAddressViaServiceBinding),
  n(E.connectTarget),

  // jsrpc DO subrequest test: caller has children (MyService + MyActor DO calls)
  n(E.jsrpcDoSubrequest, [
    n(E.myActorJsrpc),
    n(E.jsrpcGetCounter),
    n(E.jsrpcNonFunction),
  ]),

  // http-test: main test handler with subrequest children
  n(E.httpTest, [
    n(E.fetchRayId),
    n(E.fetchBodyLength),
    n(E.fetchBodyLength),
    n(E.scheduledEmpty),
    n(E.scheduledCron),
  ]),

  // http-test: standalone fetch handlers that are NOT subrequests of the main test
  // (not-found, web-socket, empty-url are separate test invocations)
  n(E.fetchNotFound),
  n(E.fetchWebSocket),
  n(E.fetchEmptyUrl),

  // queue-test: caller has subrequest children
  n(E.queueTest, [
    n(E.fetchBatch),
    n(E.fetchMsgText),
    n(E.fetchMsgBytes),
    n(E.fetchMsgJson),
    n(E.fetchMsgV8),
    n(E.queueConsumer),
  ]),

  // buffered tail worker traces
  n(E.trace),
  n(E.trace),
].sort((a, b) => a.events.localeCompare(b.events));

// Sort children in expected trees to match buildTree's sort order.
function sortTreeChildren(nodes) {
  for (const node of nodes) {
    node.children.sort((a, b) => a.events.localeCompare(b.events));
    sortTreeChildren(node.children);
  }
}
sortTreeChildren(expectedFlat);
sortTreeChildren(expectedWithPropagation);

export const test = {
  async test() {
    // HACK: The prior tests terminates once the scheduled() invocation has returned a response, but
    // propagating the outcome of the invocation may take longer. Wait briefly so this can go ahead.
    await scheduler.wait(50);

    // @all-autogates enables USER_SPAN_CONTEXT_PROPAGATION.
    const expected = unsafe.isTestAutogateEnabled()
      ? expectedWithPropagation
      : expectedFlat;

    verifyTraceIds(allInvocations);
    assert.deepStrictEqual(buildTree(allInvocations), expected);
  },
};
