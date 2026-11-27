// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';

// The chain is kept in the isolate, across instances of the Durable Object.
let rounds = 0;
let lastRound = 0;
// Aborts the Durable Object at the last round. `inListener` is whether the listener calls, rather
// than the microtask it queued.
let abortAtLastRound = (inListener) => {};

// A chain of unhandled rejections that never ends: the listener of each one queues a microtask
// that rejects the next promise.
addEventListener('unhandledrejection', () => {
  // A chain that outlives the abort stops, for the test to fail rather than hang.
  if (++rounds > lastRound) {
    return;
  }
  queueMicrotask(() => {
    abortAtLastRound(false);
    Promise.reject(new Error('next'));
  });
  abortAtLastRound(true);
});

export class Chain extends DurableObject {
  // Starts the chain, which aborts the object at round `abortAt`: from the listener if
  // `abortInListener`, else from the microtask.
  run(abortAt, abortInListener) {
    rounds = 0;
    lastRound = abortAt;
    abortAtLastRound = (inListener) => {
      if (rounds === lastRound && inListener === abortInListener) {
        this.ctx.abort('chain aborted');
      }
    };
    Promise.reject(new Error('first'));
    return new Promise(() => {});
  }

  getRounds() {
    return rounds;
  }
}
