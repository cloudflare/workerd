// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Each target must continue its caller span through an untraced Worker or Durable Object.

import * as assert from 'node:assert';

// invocationId -> { url, traceId, parentId, spans: Map(spanId -> name) }
const invocations = new Map();

export default {
  tailStream(onsetEvent, env, ctx) {
    const data = {
      url: onsetEvent.event.info?.url,
      traceId: onsetEvent.spanContext.traceId,
      parentId: onsetEvent.spanContext.spanId,
      spans: new Map(),
    };
    invocations.set(onsetEvent.invocationId, data);

    return (event) => {
      if (event.event.type === 'spanOpen') {
        data.spans.set(event.event.spanId, event.event.name);
      }
    };
  },
};

const TARGET_URLS = [
  'http://target/1',
  'http://target/2',
  'http://target-object/1',
  'http://target-object/2',
];

// Tail events arrive asynchronously; wait for both ends of every parent relationship.
function findInvocations() {
  const all = [...invocations.values()];
  const targets = TARGET_URLS.map((url) => all.find((inv) => inv.url === url));
  const callers = all.filter((inv) => !TARGET_URLS.includes(inv.url));
  if (targets.includes(undefined) || callers.length !== 1) return null;
  const caller = callers[0];
  if (!targets.every((t) => t.parentId && caller.spans.has(t.parentId))) {
    return null;
  }
  return { caller, targets };
}

export const test = {
  async test() {
    const deadline = Date.now() + 5000;
    let found = findInvocations();
    while (!found && Date.now() < deadline) {
      await scheduler.wait(10);
      found = findInvocations();
    }
    assert.ok(
      found,
      `Tail events did not show every target parented on a caller span: ${JSON.stringify(
        [...invocations.values()].map((inv) => ({
          url: inv.url,
          traceId: inv.traceId,
          parentId: inv.parentId,
          spans: [...inv.spans.entries()],
        }))
      )}`
    );

    const { caller, targets } = found;
    for (const target of targets) {
      assert.strictEqual(
        target.traceId,
        caller.traceId,
        `${target.url} is in the caller's trace`
      );
    }
    // The retained Durable Object stub must use each request's own parent.
    assert.strictEqual(
      new Set(targets.map((t) => t.parentId)).size,
      targets.length,
      `distinct parents: ${targets.map((t) => t.parentId).join(', ')}`
    );
    // JSRPC tracing can nest a fetch span under durable_object_subrequest.
    for (const target of targets) {
      const name = caller.spans.get(target.parentId);
      assert.ok(
        ['fetch', 'durable_object_subrequest'].includes(name),
        `${target.url} is parented on a subrequest span, not ${name}`
      );
    }
  },
};
