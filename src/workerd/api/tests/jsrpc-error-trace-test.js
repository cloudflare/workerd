// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

export class ThrowingService extends WorkerEntrypoint {
  throwError() {
    console.log('callee throwError called');
    throw new Error('intentional JSRPC failure');
  }

  async throwAsyncError() {
    await scheduler.wait(1);
    throw new Error('intentional async JSRPC failure');
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

    try {
      await env.ThrowingService.throwAsyncError();
      throw new Error('Expected throwAsyncError() to reject');
    } catch (error) {
      strictEqual(error.message, 'intentional async JSRPC failure');
    }
  },
};
