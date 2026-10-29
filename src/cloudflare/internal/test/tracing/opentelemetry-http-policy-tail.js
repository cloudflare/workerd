// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { createHierarchyAwareCollector } from 'instrumentation-test-helper';
const collector = createHierarchyAwareCollector();
const requests = [];
export default {
  tailStream(onset) {
    const item = { onset, spans: [], logs: [] };
    requests.push(item);
    const collect = collector.tailStream(onset);
    return (event) => {
      if (event.event.type === 'spanOpen') item.spans.push(event);
      if (event.event.type === 'log') item.logs.push(...event.event.message);
      collect(event);
    };
  },
};
export const validate = {
  async test() {
    await collector.waitForCompletion();
    let mixed = 0,
      forwarded = 0,
      redirects = 0,
      suppressed = 0;
    for (const { onset, spans, logs } of requests) {
      const remote = onset.event.info.headers
        ?.find((h) => h.name === 'traceparent')
        ?.value?.split('-')[2];
      const known = new Set([
        onset.event.spanId,
        onset.spanContext.spanId,
        remote,
        ...spans.map((s) => s.event.spanId),
      ]);
      for (const span of spans) {
        assert.ok(
          known.has(span.spanContext.spanId),
          `unknown parent ${span.spanContext.spanId}: ${span.event.name}`
        );
      }
      const path = new URL(onset.event.info.url).pathname;
      if (path === '/mixed') {
        mixed++;
        assert.strictEqual(
          spans.filter((s) => s.event.name === 'mixed-child').length,
          1
        );
      }
      if (path === '/suppress') {
        suppressed++;
        assert.strictEqual(
          spans.filter((s) => s.event.name === 'fetch').length,
          1
        );
        assert.strictEqual(
          spans.some((s) => s.event.name === 'suppressed-native'),
          false
        );
      }
      if (path === '/forward' || path === '/clone' || path === '/redirect') {
        if (onset.spanContext.traceFlags === 0) continue;
        const result = logs
          .filter((s) => s.startsWith('{'))
          .map((s) => JSON.parse(s))
          .find((s) => s.path === path);
        assert.ok(result);
        const id = result.echo.traceparent.split('-')[2];
        const fetch = spans.find(
          (s) => s.event.name === 'fetch' && s.event.spanId === id
        );
        assert.ok(fetch, `egress parent is not a fetch span: ${path}`);
        if (path === '/redirect') {
          redirects++;
          assert.strictEqual(
            spans.filter((s) => s.event.name === 'fetch').length,
            2
          );
        } else forwarded++;
      }
    }
    assert.strictEqual(mixed, 1);
    assert.ok(forwarded >= 4);
    assert.strictEqual(redirects, 1);
    assert.strictEqual(suppressed, 1);
  },
};
