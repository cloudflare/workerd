// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ContainerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';

// Example ContainerEntrypoint that extends the base class
export class MyContainer extends ContainerEntrypoint {
  async customMethod() {
    return 'custom response';
  }
}

// Test that ContainerEntrypoint can be instantiated and ping method works
export const testPingMethod = {
  async test(ctrl, env, ctx) {
    // Get a stub to MyContainer
    const id = env.MY_CONTAINER.idFromName('test-container');
    const stub = env.MY_CONTAINER.get(id);

  // Call the built-in ping method
    const pingResult = await stub.ping();
    assert.strictEqual(pingResult, 'pong', 'ping() should return "pong"');
  },
};

