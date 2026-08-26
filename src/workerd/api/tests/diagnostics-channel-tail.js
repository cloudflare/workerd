// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker that collects events from the trigger worker and
// verifies that non-serializable diagnostics_channel payloads produce a
// fallback placeholder message instead of a trace exception.

import * as assert from 'node:assert';

let allEvents = [];

export default {
  tailStream(event) {
    allEvents.push(event.event);
    return (event) => {
      allEvents.push(event.event);
    };
  },
};

export const test = {
  async test() {
    // Brief wait for streaming tail events to arrive (see tail-worker-test.js).
    await scheduler.wait(50);

    const diagEvents = allEvents.filter((e) => e.type === 'diagnosticChannel');

    // 1. Serializable message is forwarded as-is.
    const serEvent = diagEvents.find((e) => e.channel === 'test:serializable');
    assert.ok(serEvent, 'expected test:serializable diagnostic channel event');
    assert.deepStrictEqual(serEvent.message, { key: 'value' });

    // 2. Non-serializable message gets a fallback placeholder string.
    const nonSerEvent = diagEvents.find(
      (e) => e.channel === 'test:non-serializable'
    );
    assert.ok(
      nonSerEvent,
      'expected test:non-serializable diagnostic channel event'
    );
    assert.strictEqual(
      nonSerEvent.message,
      '[[ Diagnostic event was not serializable ]]'
    );

    // 3. No trace exceptions from serialization failures.
    // Before the fix, the runtime would call tracer.addException() with
    // "Failed to publish diagnostics channel message: ..." for every
    // non-serializable payload.
    const serFailureExceptions = allEvents.filter(
      (e) =>
        e.type === 'exception' &&
        e.message?.includes?.('Failed to publish diagnostics channel message')
    );
    assert.strictEqual(
      serFailureExceptions.length,
      0,
      'non-serializable diagnostic channel messages must not produce trace exceptions'
    );
  },
};
