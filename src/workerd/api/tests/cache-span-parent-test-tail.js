// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Checks the traces from cache-span-parent-test.js. Each of its Cache API operations reaches
// Backend as its own fetch invocation, which must continue the caller's trace under the span of
// the operation that sent it.

import { ok, strictEqual } from 'node:assert';

// One entry per traced invocation: the caller's test(), which has no URL, and a Backend fetch for
// each operation, identified by its URL. parentId is the span the invocation started under, spans
// maps each span the invocation opened to its name, and ended settles once its tail stream
// delivers the outcome, the stream's last event.
const invocations = [];
const operations = ['match', 'put', 'delete'];
const urls = ['default', 'named'].flatMap((name) =>
  operations.map((operation) => `https://cache.test/${name}/${operation}`)
);

export default {
  tailStream(onset) {
    const { promise: ended, resolve: end } = Promise.withResolvers();
    const invocation = {
      url: onset.event.info?.url,
      traceId: onset.spanContext.traceId,
      parentId: onset.spanContext.spanId,
      spans: new Map(),
      ended,
    };
    invocations.push(invocation);
    return (event) => {
      if (event.event.type === 'spanOpen') {
        invocation.spans.set(event.event.spanId, event.event.name);
      } else if (event.event.type === 'outcome') {
        end();
      }
    };
  },
};

export const test = {
  async test() {
    // Tail events can still be in flight when the caller's test() returns. Wait until every tail
    // stream has delivered its outcome, including streams that start while waiting.
    let count;
    do {
      count = invocations.length;
      await Promise.all(invocations.map((invocation) => invocation.ended));
    } while (invocations.length > count);

    const caller = invocations.find(
      (invocation) => invocation.url === undefined
    );
    ok(caller, 'Missing the caller invocation');
    const parentIds = new Set();
    for (const url of urls) {
      const target = invocations.find((invocation) => invocation.url === url);
      ok(target, `Missing the Backend invocation for ${url}`);
      // The invocation continues the caller's trace, under the span of the operation that sent it
      // rather than a new root or another operation's span.
      strictEqual(target.traceId, caller.traceId, url);
      const operation = new URL(url).pathname.split('/')[2];
      strictEqual(caller.spans.get(target.parentId), `cache_${operation}`, url);
      parentIds.add(target.parentId);
    }
    // Each operation has its own span, so no two Backend invocations share a parent.
    strictEqual(parentIds.size, urls.length);
  },
};
