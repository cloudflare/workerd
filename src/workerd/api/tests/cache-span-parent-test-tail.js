// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, strictEqual } from 'node:assert';

const invocations = [];
const operations = ['match', 'put', 'delete'];
const urls = ['default', 'named'].flatMap((name) =>
  operations.map((operation) => `https://cache.test/${name}/${operation}`)
);

export default {
  tailStream(onset) {
    const invocation = {
      url: onset.event.info?.url,
      traceId: onset.spanContext.traceId,
      parentId: onset.spanContext.spanId,
      spans: new Map(),
    };
    invocations.push(invocation);
    return (event) => {
      if (event.event.type === 'spanOpen') {
        invocation.spans.set(event.event.spanId, event.event.name);
      }
    };
  },
};

function findRelationships() {
  const caller = invocations.find((invocation) => invocation.url === undefined);
  const targets = urls.map((url) =>
    invocations.find((invocation) => invocation.url === url)
  );
  if (
    !caller ||
    targets.some((target) => !target || !caller.spans.has(target.parentId))
  ) {
    return null;
  }
  return { caller, targets };
}

export const test = {
  async test() {
    const deadline = Date.now() + 5000;
    let found = findRelationships();
    while (!found && Date.now() < deadline) {
      await scheduler.wait(10);
      found = findRelationships();
    }
    ok(
      found,
      `Missing Cache API parent relationships: ${JSON.stringify(
        invocations.map((invocation) => ({
          ...invocation,
          spans: [...invocation.spans.entries()],
        }))
      )}`
    );
    const { caller, targets } = found;
    strictEqual(
      new Set(targets.map((target) => target.parentId)).size,
      urls.length
    );
    for (const target of targets) {
      strictEqual(target.traceId, caller.traceId, target.url);
      const operation = new URL(target.url).pathname.split('/')[2];
      strictEqual(
        caller.spans.get(target.parentId),
        `cache_${operation}`,
        target.url
      );
    }
  },
};
