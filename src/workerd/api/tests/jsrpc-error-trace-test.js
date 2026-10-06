// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

export class ThrowingService extends WorkerEntrypoint {
  async throwError() {
    console.log('callee throwError called');
    throw new Error('intentional JSRPC failure');
  }

  async neverResolves() {
    console.log('callee neverResolves called');
    return new Promise(() => {});
  }
}

export default {
  async test(_controller, env, _ctx) {
    try {
      await env.ThrowingService.throwError();
      throw new Error('Expected throwError() to reject');
    } catch (error) {
      strictEqual(error.message, 'intentional JSRPC failure');
    }

    const pending = env.ThrowingService.neverResolves();
    pending.catch(() => {});
    await scheduler.wait(500);
  },
};
