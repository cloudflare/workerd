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
    // Opens the session.
    const counter = await env.CounterService.getCounter();

    // Reuses the session opened above, so this is delivered to the same callee invocation.
    const result = await counter.increment(5);
    if (result !== 5) {
      throw new Error(`Expected 5, got ${result}`);
    }
  },
};
