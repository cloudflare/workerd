// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Garbage collection: a target keeps its listeners alive, and nothing else
// does. In particular a long-lived {signal} keeps neither a dead target nor
// its listeners alive. Requires --expose-gc and FinalizationRegistry
// (enable_weak_ref), which every cell config sets.

import { strictEqual } from 'node:assert';

async function collectGarbage() {
  for (let i = 0; i < 3; i++) {
    gc();
    await scheduler.wait(5);
  }
}

// Returns a FinalizationRegistry and a function reading how many of its
// registrations have been collected.
function trackCollection() {
  let collected = 0;
  const registry = new FinalizationRegistry(() => {
    collected++;
  });
  return [registry, () => collected];
}

export const signalDoesNotRetainDeadTargets = {
  async test() {
    const controller = new AbortController();
    const [registry, collected] = trackCollection();
    // A separate frame, so no target lingers in this one's slots.
    (() => {
      for (let i = 0; i < 4; i++) {
        const target = new EventTarget();
        const listener = () => {};
        target.addEventListener('x', listener, { signal: controller.signal });
        registry.register(target, 'target');
        registry.register(listener, 'listener');
      }
    })();
    await collectGarbage();
    strictEqual(collected(), 8);
    // There is nothing left to remove.
    controller.abort();
  },
};

export const targetRetainsItsListeners = {
  async test() {
    const target = new EventTarget();
    const [registry, collected] = trackCollection();
    let calls = 0;
    (() => {
      const listener = () => {
        calls++;
      };
      const object = {
        handleEvent() {
          calls++;
        },
      };
      target.addEventListener('x', listener);
      target.addEventListener('x', object);
      registry.register(listener, 'listener');
      registry.register(object, 'object');
    })();
    await collectGarbage();
    strictEqual(collected(), 0);
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 2);
  },
};

// Once a listener's registration ends (removed, run once, aborted), the
// target no longer keeps it alive.
export const endedRegistrationsReleaseTheirListeners = {
  async test() {
    const target = new EventTarget();
    const controller = new AbortController();
    const [registry, collected] = trackCollection();
    (() => {
      const removed = () => {};
      const once = () => {};
      const aborted = () => {};
      target.addEventListener('x', removed);
      target.addEventListener('x', once, { once: true });
      target.addEventListener('x', aborted, { signal: controller.signal });
      registry.register(removed, 'removed');
      registry.register(once, 'once');
      registry.register(aborted, 'aborted');
      target.removeEventListener('x', removed);
      target.dispatchEvent(new Event('x'));
      controller.abort();
    })();
    await collectGarbage();
    strictEqual(collected(), 3);
  },
};
