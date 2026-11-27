// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual } from 'node:assert';

const ABORT_AT = 100;

// Runs, in a Durable Object, a chain of unhandled rejections that only ends when the object
// aborts. Returns how many rounds of the chain ran, as seen by the next instance of the object.
async function roundsOfAbortedChain(env, abortInListener) {
  const id = env.CHAIN.idFromName('chain');
  await rejects(env.CHAIN.get(id).run(ABORT_AT, abortInListener), {
    message: 'chain aborted',
  });
  return await env.CHAIN.get(id).getRounds();
}

// The listener has queued the microtask that rejects the next promise when it aborts the object:
// that microtask must not run in the next request.
export const abortInListenerEndsChain = {
  async test(ctrl, env) {
    strictEqual(await roundsOfAbortedChain(env, true), ABORT_AT);
  },
};

export const abortInMicrotaskEndsChain = {
  async test(ctrl, env) {
    strictEqual(await roundsOfAbortedChain(env, false), ABORT_AT);
  },
};
