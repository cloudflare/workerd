// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

// More than the old generation's allocation limit allows, both before the first full GC
// (--initial-old-space-size in the .wd-test) and after one of a nearly empty heap.
const OVER_LIMIT_MB = 24;

// Lets the thread go idle `attempts` times, each time after a request to the target has
// completed, and returns whether a full GC ran in the target's isolate.
async function collectsWhenIdle(target, attempts) {
  for (let i = 0; i < attempts; i++) {
    await scheduler.wait(10);
    if (await target.isTrackedCollected()) {
      return true;
    }
  }
  return false;
}

export const gcWhenIdle = {
  async test(ctrl, env) {
    const target = env.TARGET;

    // An isolate with a request in flight is not idle, however much garbage it has. This also
    // shows that nothing else collects the garbage: promoting objects does not check the limit.
    const entered = Promise.withResolvers();
    const released = Promise.withResolvers();
    const held = target.hold(() => {
      entered.resolve();
      return released.promise;
    });
    await entered.promise;
    await target.makeOldGarbage(OVER_LIMIT_MB);
    assert.strictEqual(await collectsWhenIdle(target, 5), false);

    // This is the isolate's first idle point, which ends V8's loading mode if it is in it.
    released.resolve();
    await held;
    const first = env.FIRST_IDLE_GC;
    assert.strictEqual(await collectsWhenIdle(target, first ? 100 : 5), first);

    // An idle isolate whose old generation is below the limit is left alone.
    await target.makeOldGarbage(0);
    assert.strictEqual(await collectsWhenIdle(target, 5), false);

    // Later idle points check the limit unless the NO_IDLE_GC autogate is on.
    await target.makeOldGarbage(OVER_LIMIT_MB);
    const later = env.LATER_IDLE_GC;
    assert.strictEqual(await collectsWhenIdle(target, later ? 100 : 5), later);
  },
};
