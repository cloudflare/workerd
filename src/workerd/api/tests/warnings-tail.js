// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// ================================================================================================
// Expected warnings registry.
//
// Add new entries here when introducing logWarning() calls. Each entry is a substring that must
// appear in at least one warn-level tail event's message. The trigger worker (warnings-test.js)
// must exercise a code path that produces each warning.
//
// NOTE: Some logWarning() calls go through jsg::Lock::logWarning() instead of
// IoContext::logWarning(). The jsg path does not currently emit to the tracer, so those warnings
// are not observable via tail workers and cannot be tested here (e.g. .text() on non-text body).
const EXPECTED_WARNINGS = [
  // From Body::Body (http.c++) — FormData body with a custom Content-Type header.
  'FormData body was provided',
  // From Response constructor (http.c++) — null-body status with zero-length body.
  'Constructing a Response with a null body status',
];
// ================================================================================================

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
    // HACK: Tail events from the trigger worker's fetch invocation are delivered asynchronously
    // across service boundaries. There is no JS-level synchronization primitive to await their
    // arrival — a plain `await promise` doesn't drive the I/O loop that delivers them. We must
    // use scheduler.wait() to yield while the runtime processes pending I/O.
    const TIMEOUT_MS = 5000;
    const POLL_MS = 10;
    const deadline = Date.now() + TIMEOUT_MS;

    const unseen = () =>
      EXPECTED_WARNINGS.filter(
        (substring) =>
          !allEvents.some(
            (e) =>
              e.type === 'log' &&
              e.level === 'warn' &&
              e.message?.[0]?.includes(substring)
          )
      );

    while (unseen().length > 0 && Date.now() < deadline) {
      await scheduler.wait(POLL_MS);
    }

    const missing = unseen();
    assert.strictEqual(
      missing.length,
      0,
      `${missing.length} expected warning(s) were not observed after ${TIMEOUT_MS}ms:\n` +
        missing.map((s) => `  - "${s}"`).join('\n')
    );
  },
};
