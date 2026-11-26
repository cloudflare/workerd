// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entrypoints whose test() passes or fails after awaiting. `workerd test` runs only the
// entrypoints that export a test(), in the order of their names.

export const failsAsynchronously = {
  async test() {
    await scheduler.wait(1);
    throw new Error('async test failure');
  },
};

export const passes = {
  async test(ctrl, env, ctx) {
    ctx.waitUntil(scheduler.wait(1));
    await scheduler.wait(1);
  },
};
