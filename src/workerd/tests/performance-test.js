// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

if (typeof globalThis.performance === 'undefined') {
  throw new Error('performance is not defined');
}

if (globalThis.performance.timeOrigin !== 0.0) {
  throw new Error('performance.timeOrigin is not 0.0');
}

if (globalThis.performance.now() !== 0.0) {
  throw new Error('performance.now() is not 0.0');
}

export const test = {
  async test(ctrl, env, ctx) {
    const start = performance.now();
    // There should be at least some time elapsed.
    if (start == 0.0) {
      throw new Error('performance.now() is 0.0');
    }
    await scheduler.wait(10);
    if (start == performance.now()) {
      throw new Error('performance.now() is not increasing');
    }
  },
};
