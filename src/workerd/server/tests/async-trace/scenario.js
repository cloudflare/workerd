// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Traced by async-trace-test.sh; check.js asserts on the resulting trace.

export default {
  async fetch() {
    await scheduler.wait(1);
    return new Response('hello');
  },

  async test(ctrl, env) {
    await new Promise((resolve) => setTimeout(resolve, 1));
    queueMicrotask(() => {});
    const response = await env.SELF.fetch('http://scenario/');
    if ((await response.text()) !== 'hello') {
      throw new Error('unexpected response');
    }
  },
};
