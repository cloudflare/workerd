// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

// Once a queueMicrotask() task has run, the runtime no longer references it, so a minor GC frees it
// while it is still in the young generation.
export const taskFreedByMinorGcAfterRunning = {
  async test() {
    let ran = false;
    let ref;
    (() => {
      // Only the WeakRef and the microtask queue reference the task.
      const task = () => {
        ran = true;
      };
      ref = new WeakRef(task);
      queueMicrotask(task);
    })();

    // Return to the event loop, so that the microtask runs and the WeakRef stops keeping the task
    // alive: a WeakRef holds its target strongly until the end of the job that created it.
    await scheduler.wait(0);
    assert.ok(ran);

    globalThis.gc({ type: 'minor' });
    assert.strictEqual(ref.deref(), undefined);
  },
};
