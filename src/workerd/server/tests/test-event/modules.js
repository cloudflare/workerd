// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entrypoints whose test() passes, fails after awaiting, or is gone. `workerd test` runs only
// the entrypoints that export a test(), in the order of their names, and reads test() from the
// export only when its test runs, so `deletesTest` can remove `noTest`'s before it runs.

export const deletesTest = {
  test() {
    delete noTest.test;
  },
};

export const failsAsynchronously = {
  async test() {
    await scheduler.wait(1);
    throw new Error('async test failure');
  },
};

export const noTest = {
  test() {
    throw new Error('deletesTest did not run first');
  },
};

export const passes = {
  async test(ctrl, env, ctx) {
    ctx.waitUntil(scheduler.wait(1));
    await scheduler.wait(1);
  },
};
