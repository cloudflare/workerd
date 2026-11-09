// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker that checks the outcome of each JSRPC session the tested service
// received.
import assert from 'node:assert';
import { scheduler } from 'node:timers/promises';

// Each tail worker has the same compatibility flags as the service it tails, so this says whether
// that service has the flag, including in the all-compat-flags variant.
const CANCELS =
  globalThis.Cloudflare.compatibilityFlags['rpc_dispose_cancels_call'];

// The top-level methods whose calls the tests cancel by disposing their promise, when the service
// calling them, itself, has the flag. Each of these calls has a session of its own.
const SELF_CANCELED_METHODS = new Set(['hang', 'hangWithCallback']);
const SELF_CANCELED_SESSIONS = 3;

// The top-level method that a service with the flag calls on the service without it, and cancels.
const CROSS_CANCELED_METHOD = 'hangWithProbe';

// The sessions that have ended, as { method, outcome }, and how many are still open.
const sessions = [];
let openSessions = 0;

export default {
  tailStream(onsetEvent) {
    if (onsetEvent.event.info?.type !== 'jsrpc') return;
    ++openSessions;
    // The session's top-level method. Calls on stubs the session returns add their own
    // `jsrpc.method` attributes later.
    let method;
    return (event) => {
      if (event.event.type === 'attributes') {
        for (const { name, value } of event.event.info) {
          if (name === 'jsrpc.method') method ??= value;
        }
      } else if (event.event.type === 'outcome') {
        --openSessions;
        sessions.push({ method, outcome: event.event.outcome });
      }
    };
  },
};

function expectedOutcome(method) {
  if (method === CROSS_CANCELED_METHOD) return 'canceled';
  return CANCELS && SELF_CANCELED_METHODS.has(method) ? 'canceled' : 'ok';
}

export const outcomes = {
  async test() {
    // A canceled call ends its session right away, even though the method keeps running, so every
    // session ends promptly.
    const selfCanceledSessions = () =>
      sessions.filter(({ method }) => SELF_CANCELED_METHODS.has(method));
    for (
      let i = 0;
      i < 100 &&
      (openSessions > 0 ||
        selfCanceledSessions().length < SELF_CANCELED_SESSIONS);
      ++i
    ) {
      await scheduler.wait(10);
    }

    assert.strictEqual(openSessions, 0);
    assert.strictEqual(selfCanceledSessions().length, SELF_CANCELED_SESSIONS);
    for (const { method, outcome } of sessions) {
      assert.strictEqual(
        outcome,
        expectedOutcome(method),
        `outcome of ${method}()`
      );
    }
  },
};
