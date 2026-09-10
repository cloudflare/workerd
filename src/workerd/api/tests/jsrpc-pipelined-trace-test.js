// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Test for JSRPC tracing: the callee invocation must be parented to the caller's first per-call
// jsRpcCall span, and every callee call must link to its corresponding caller span.
//
// A single JSRPC session carries many calls: `getCounter()` opens the session, and the calls on
// the returned stub reuse it. The callee therefore has one invocation (one onset) covering every
// call, so the onset's parent alone can't attribute work to an individual call. The caller sends
// its per-call span identity with each call so the callee can attribute every dispatch precisely.
//
// The parent/child relationships are asserted in the tail worker's test() handler (see
// jsrpc-pipelined-trace-test-tail.js).

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
