// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  collectedEvents,
  spanIdentities as sharedSpanIdentities,
} from 'opentelemetry-instrumentation-test';

export { default } from 'opentelemetry-instrumentation-test';

export const spanIdentities = {
  async test() {
    // Two real-package invocations and the incompatible-copy fixture each stream a tail.
    const events = await collectedEvents(3);
    const opens = events.filter((event) => event.event.type === 'spanOpen');
    const parents = opens.filter(
      (event) => event.event.name === 'real-api-parent'
    );
    // The 1.9 and 1.10 fixtures each run realPackage in a separate invocation.
    assert.strictEqual(
      parents.length,
      2,
      'both API copies must emit a parent span'
    );
    assert.strictEqual(
      new Set(parents.map((event) => event.invocationId)).size,
      2
    );
    for (const parent of parents) {
      const child = opens.find(
        (event) =>
          event.event.name === 'real-api-child' &&
          event.invocationId === parent.invocationId
      );
      assert.ok(child, 'both API copies must emit a child span');
      assert.strictEqual(child.spanContext.spanId, parent.event.spanId);
      assert.strictEqual(child.spanContext.traceId, parent.spanContext.traceId);
    }
    assert.ok(
      opens.some((event) => event.event.name === 'compatible-span'),
      'the incompatible-copy fixture must emit compatible-span'
    );
    await sharedSpanIdentities.test();
  },
};
