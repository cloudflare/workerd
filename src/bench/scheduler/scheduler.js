// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// scheduler.wait(): the global, a wait that the timer ends, one that a signal
// ends, and one that is refused. A read of the global costs about as much as
// the runner's overhead per call, so that case does COUNT of them.

const COUNT = 16;

// The top level reads the method, as feature detection does, so that the
// startup cases include making the object.
const kWait = scheduler.wait;

export const reads = {
  bench(b) {
    b.blackBox(kWait);
    b.run(`scheduler x${COUNT}`, () => {
      let value;
      for (let i = 0; i < COUNT; i++) value = scheduler;
      b.blackBox(value);
    });
    b.run(`scheduler.wait x${COUNT}`, () => {
      let value;
      for (let i = 0; i < COUNT; i++) value = scheduler.wait;
      b.blackBox(value);
    });
  },
};

export const waits = {
  bench(b) {
    b.run('await scheduler.wait(0)', async () => {
      await scheduler.wait(0);
    });
    const { signal } = new AbortController();
    b.run('await scheduler.wait(0, { signal })', async () => {
      await scheduler.wait(0, { signal });
    });
    // Four waits at once, the timer ending them together.
    b.run('await 4 x scheduler.wait(0)', async () => {
      await Promise.all([
        scheduler.wait(0),
        scheduler.wait(0),
        scheduler.wait(0),
        scheduler.wait(0),
      ]);
    });
  },
};

export const aborts = {
  bench(b) {
    const aborted = AbortSignal.abort(new Error('aborted'));
    b.run('scheduler.wait(1000, { signal: aborted })', async () => {
      try {
        await scheduler.wait(1000, { signal: aborted });
      } catch (error) {
        b.blackBox(error);
      }
    });
    b.run('scheduler.wait(1000, { signal }), then abort', async () => {
      const controller = new AbortController();
      const waiting = scheduler.wait(1000, { signal: controller.signal });
      controller.abort();
      try {
        await waiting;
      } catch (error) {
        b.blackBox(error);
      }
    });
  },
};

export const refused = {
  bench(b) {
    b.run('scheduler.wait() without a delay', async () => {
      try {
        await scheduler.wait();
      } catch (error) {
        b.blackBox(error);
      }
    });
    const wait = kWait;
    b.run('wait() without its receiver', () => {
      try {
        wait(0);
      } catch (error) {
        b.blackBox(error);
      }
    });
  },
};
