// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Traced by async-trace-shutdown-test.sh. The Durable Object's context outlives the test, so it is
// still open when workerd shuts down.

import { DurableObject } from 'cloudflare:workers';

export class Counter extends DurableObject {
  async increment() {
    const value = ((await this.ctx.storage.get('n')) ?? 0) + 1;
    await this.ctx.storage.put('n', value);
    return value;
  }
}

export const test = {
  async test(ctrl, env) {
    const stub = env.COUNTER.get(env.COUNTER.idFromName('a'));
    if ((await stub.increment()) !== 1) throw new Error('unexpected count');
  },
};
