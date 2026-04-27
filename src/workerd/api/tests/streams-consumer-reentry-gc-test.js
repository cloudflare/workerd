// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for EDGEWORKER-RUNTIME-H40: SIGSEGV during ConsumerImpl
// close drain when re-entrant controller.error() destroys the consumer
// mid-iteration.
//
// The mechanism: during maybeDrainAndSetState's close-drain loop, each
// resolveAsDone call wraps ReadResult via wrapOpaque, which creates a V8
// object. V8's promise resolution machinery checks for a "then" property
// on the resolved value. A malicious Object.prototype.then getter can
// re-enter C++ and call controller.error(), which:
//   1) Rejects all remaining readRequests (error path in maybeDrainAndSetState)
//   2) Transitions ConsumerImpl state to Errored (destroys Ready struct)
//   3) Notifies stateListener -> doError -> destroys ValueReadable -> Consumer
// The outer close-drain loop then dereferences freed memory -> SIGSEGV.
//
// The fix in queue.h extracts readRequests to local ownership before
// resolving/rejecting and uses selfRef WeakRef to guard member access.

import { strictEqual } from 'node:assert';

export default {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
      pull(c) {
        return new Promise(() => {});
      }, // never resolves -> reads stay pending
    });

    // Let start() onSuccess microtask run.
    await Promise.resolve();

    const reader = rs.getReader();
    // Queue 3 pending readRequests in the consumer's RingBuffer.
    reader.read().catch(() => {});
    reader.read().catch(() => {});
    reader.read().catch(() => {});

    let armed = false;
    const noopThen = function (resolve, reject) {
      /* never settle */
    };

    // Install a trap on Object.prototype.then that re-enters the stream
    // controller when V8 tries to resolve the first pending read.
    // V8's promise resolution checks for a "then" property on the resolved
    // value (thenable detection). If it finds one, it queues a microtask
    // instead of immediately fulfilling. Our getter uses this window to
    // call controller.error(), which re-enters ConsumerImpl and destroys
    // the readRequests being iterated.
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        if (armed) {
          armed = false;
          try {
            controller.error(new Error('boom'));
          } catch {
            // Intentionally empty
          }
          return noopThen;
        }
        return undefined;
      },
    });

    armed = true;
    try {
      controller.close();
    } catch {
      // close may throw due to the re-entrant error — that's expected
    }
    armed = false;
    delete Object.prototype.then;

    // If we get here without SIGSEGV, the fix works.
    strictEqual(true, true, 'survived re-entrant error during close drain');
  },
};
