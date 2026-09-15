// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Test for JSRPC tracing: each logical server call must be parented to its matching client call.
// Promise-pipelined calls remain children of the call whose promise they use, while calls on an
// already-resolved stub use the current async context. The transport session is not a user span.
// The relationships are asserted in jsrpc-pipelined-trace-test-tail.js.

import { WorkerEntrypoint, RpcTarget } from 'cloudflare:workers';

class Counter extends RpcTarget {
  #value = 0;

  increment(amount) {
    this.#value += amount;
    return this.#value;
  }

  incrementDuplicate(amount) {
    this.#value += amount;
    return this.#value;
  }
}

export class CounterService extends WorkerEntrypoint {
  // Returns a stub. Calls the caller subsequently makes on it reuse this same session, and so are
  // delivered to this same invocation.
  async getCounter() {
    return new Counter();
  }
}

export default {
  async test(controller, env, ctx) {
    // Pipeline increment before resolving the stub returned by getCounter.
    // Then duplicate the resolved stub to verify it retains the same ancestry.
    const counterPromise = env.CounterService.getCounter();
    const incrementPromise = counterPromise.increment(5);
    const counter = await counterPromise;
    const result = await incrementPromise;
    const duplicateResult = await counter.dup().incrementDuplicate(2);
    if (result !== 5 || duplicateResult !== 7) {
      throw new Error(
        `Expected results 5 and 7, got ${result} and ${duplicateResult}`
      );
    }
  },
};
