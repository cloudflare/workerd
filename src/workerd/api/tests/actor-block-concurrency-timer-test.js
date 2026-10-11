// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests that a blockConcurrencyWhile() callback's timers fire while an earlier timer, set
// outside the critical section, waits for the input gate. The critical section holds the gate
// until its own timer fires, so the earlier timer must not hold back later ones.
import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class TimerTester extends DurableObject {
  async run() {
    const events = [];
    const outer = scheduler.wait(10).then(() => events.push('outer'));
    await this.ctx.blockConcurrencyWhile(async () => {
      await scheduler.wait(200);
      events.push('critical section');
    });
    await outer;
    return events;
  }
}

export default {
  async test(_request, env) {
    const stub = env.ns.get(env.ns.idFromName('timer'));
    assert.deepStrictEqual(await stub.run(), ['critical section', 'outer']);
  },
};
