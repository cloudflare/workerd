// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker for rpc-stub-gc-foreign-span-test. Attaching it makes the trigger worker's
// user spans observed, which is what puts a user span into the async context frame. It also checks
// that the undisposed-stub warning, logged under the other request's frame, still reaches the tail.

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

export const undisposedStubWarningReachesTail = {
  async test() {
    // Tail events are delivered asynchronously across service boundaries, so poll for them (see
    // warnings-tail.js).
    const TIMEOUT_MS = 5000;
    const POLL_MS = 10;
    const deadline = Date.now() + TIMEOUT_MS;

    const findWarning = () =>
      allEvents.find(
        (e) =>
          e.type === 'log' &&
          e.level === 'warn' &&
          e.message?.[0]?.includes('An RPC stub was not disposed properly')
      );

    while (findWarning() === undefined && Date.now() < deadline) {
      await scheduler.wait(POLL_MS);
    }

    assert.ok(
      findWarning(),
      `undisposed RPC stub warning was not observed after ${TIMEOUT_MS}ms`
    );
  },
};
