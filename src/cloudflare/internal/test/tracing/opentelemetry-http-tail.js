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
      if (event.event.type === 'log') item.logs.push(event.event.message);
      collect(event);
    };
  },
};

export const verifyHttp = {
  async test() {
    await collector.waitForCompletion();
    let checked = 0;
    for (const item of requests) {
      for (const messages of item.logs) {
        const text = messages.find((message) => message.startsWith('{'));
        if (!text) continue;
        const result = JSON.parse(text);
        const onset = item.onset;
        if (!result.enabled) {
          assert.strictEqual(onset.spanContext.spanId, undefined);
          continue;
        }
        checked++;
        if (
          result.path.startsWith('/valid-') ||
          result.path === '/future' ||
          result.path === '/bad-state'
        ) {
          assert.strictEqual(
            onset.spanContext.traceId,
            'a3ce929d0e0e47364bf92f3577b34da6'
          );
          assert.strictEqual(onset.spanContext.spanId, 'c0ffee0ba902b7aa');
        } else {
          assert.strictEqual(onset.spanContext.spanId, undefined);
          assert.strictEqual(onset.spanContext.traceFlags, undefined);
        }
        const propagatedId = result.echo.traceparent.split('-')[2];
        const fetch = item.spans.find(
          (event) =>
            event.event.name === 'fetch' && event.event.spanId === propagatedId
        );
        if (result.path !== '/valid-00') {
          assert.ok(
            fetch,
            `propagated parent must be the fetch span: ${result.path}`
          );
          const parent = item.spans.find(
            (event) => event.event.name === 'http-parent'
          );
          assert.ok(parent);
          assert.strictEqual(fetch.spanContext.spanId, parent.event.spanId);
          assert.strictEqual(parent.spanContext.spanId, onset.event.spanId);
        } else {
          assert.strictEqual(
            item.spans.some((event) => event.event.name === 'http-parent'),
            false
          );
        }
      }
    }
    assert.ok(checked >= 17);
  },
};
