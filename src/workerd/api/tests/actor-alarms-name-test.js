// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import unsafe from 'workerd:unsafe';

// Durable Object used to verify that `ctx.id.name` is available inside the alarm handler even when
// the actor was evicted between scheduling the alarm and the alarm firing. The alarm handler
// records the name it observed into storage so the test can read it back afterwards.
export class AlarmNameObject {
  constructor(ctx, env) {
    this.ctx = ctx;
  }

  async fetch(req) {
    const url = new URL(req.url);
    switch (url.pathname) {
      case '/setup': {
        // Schedule an alarm far enough in the future that we can evict the actor before it fires.
        await this.ctx.storage.setAlarm(Date.now() + 1000);
        return new Response('ok');
      }
      case '/read': {
        const seen = await this.ctx.storage.get('nameSeenInAlarm');
        return Response.json({ seen: seen ?? null });
      }
      default:
        return new Response('not found', { status: 404 });
    }
  }

  async alarm() {
    // Record what `ctx.id.name` was during alarm execution. Before the fix this came back
    // `undefined` for an evicted, name-derived actor because the reconstructed ID dropped the name.
    const name = this.ctx.id.name;
    await this.ctx.storage.put('nameSeenInAlarm', name ?? '<undefined>');
  }
}

export const test = {
  async test(ctrl, env, ctx) {
    const name = 'my-named-actor';

    // Schedule an alarm on an actor obtained via getByName(). We consume the response body so the
    // request fully drains, allowing the actor to be evicted below.
    await (await env.NS.getByName(name).fetch('http://foo/setup')).text();

    // Evict the actor from memory, simulating the runtime tearing it down when it goes idle. This
    // await is deterministic: unsafe.evict() only resolves once the actor has actually been torn
    // down. The pending alarm remains scheduled, so when it fires it will reconstruct a fresh actor
    // purely from persisted state -- exactly the situation where the name used to be lost.
    await unsafe.evict(env.NS.getByName(name));

    // Now wait for the alarm's real-time timer (set to +1000ms above) to fire against the
    // reconstructed (cold) actor. This sleep is unavoidable: eviction is already synchronous, but
    // there is no way to be notified that the alarm fired without touching the actor, and touching
    // it before the alarm fires would warm it back up (with its name intact) and mask the bug. We
    // sleep well past the 1000ms alarm delay to stay robust under CI load.
    await new Promise((resolve) => setTimeout(resolve, 5000));

    // Read back what the alarm handler observed.
    const res = await env.NS.getByName(name).fetch('http://foo/read');
    const { seen } = await res.json();

    assert.strictEqual(
      seen,
      name,
      `ctx.id.name inside the alarm handler should be "${name}", but was ${JSON.stringify(seen)}`
    );
  },
};
