// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Regression test for a GC leak in tail events: TraceItem's fetch Request
// held its `cf` object in an untraced strong root, and getCf() returns that
// same mutable object. A tail handler that made `cf` reference the request
// (or anything reaching its wrapper) closed an uncollectable JS<->C++ cycle:
// cf root -> cf -> Request wrapper -> C++ Request -> Detail -> cf root.
// The fix traces `cf` from Request::visitForGc so V8 can collect the cycle.
import * as assert from 'node:assert';

// Module-level state may only hold WeakRefs, or it would pin the objects
// under test.
const cfRefs = [];
const reqRefs = [];

export default {
  async fetch(request, env) {
    return new Response('ok');
  },

  tail(events) {
    for (const event of events) {
      const req = event.event?.request;
      if (!req?.cf) continue;
      // Proves the cf blob propagated through the service binding into the
      // trace; without this the GC assertions would vacuously pass.
      assert.strictEqual(req.cf.marker, 'tail-cf-gc');
      // Mutate the shared cf object to reference the request, closing the
      // would-be-uncollectable cycle through the untraced strong root.
      req.cf.self = req;
      cfRefs.push(new WeakRef(req.cf));
      reqRefs.push(new WeakRef(req));
    }
  },
};

async function awaitGc() {
  // Multiple GC passes with yields between them; gives the cycle collector
  // room to reclaim and avoids the conservative stack scanner pinning the
  // most recent allocation.
  for (let i = 0; i < 4; i++) {
    await scheduler.wait(0);
    globalThis.gc();
  }
}

export const tailRequestCfCollects = {
  async test(ctrl, env) {
    for (let i = 0; i < 8; i++) {
      const res = await env.SERVICE.fetch('http://example.com/', {
        cf: { marker: 'tail-cf-gc' },
      });
      assert.strictEqual(await res.text(), 'ok');
    }

    // Tail events are delivered asynchronously after each invocation; poll
    // until they have all arrived.
    for (let i = 0; i < 100 && cfRefs.length < 8; i++) {
      await scheduler.wait(50);
    }
    assert.strictEqual(
      cfRefs.length,
      8,
      `expected 8 traced requests with cf, got ${cfRefs.length}`
    );

    await awaitGc();
    let alive = 0;
    for (const ref of [...cfRefs, ...reqRefs]) {
      if (ref.deref() !== undefined) alive++;
    }
    // Allow a couple of stragglers: the conservative stack scanner can keep
    // the most recently touched pair rooted for an extra cycle. The leak
    // under test would keep all of them alive.
    assert.ok(
      alive <= 2,
      `expected traced request cf cycles to be collected, ` +
        `${alive} of ${cfRefs.length + reqRefs.length} still alive`
    );
  },
};
