// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { RpcTarget } from 'cloudflare:workers';
import assert from 'node:assert';

// Minimal mock step for the flag-disabled path.
class MockStep extends RpcTarget {
  calls = [];

  async do(...args) {
    const callback = typeof args[1] === 'function' ? args[1] : args[2];
    this.calls.push({ method: 'do', name: args[0] });
    return await callback();
  }

  async sleep(name) {
    this.calls.push({ method: 'sleep', name });
  }

  async sleepUntil(name) {
    this.calls.push({ method: 'sleepUntil', name });
  }

  async waitForEvent(name) {
    this.calls.push({ method: 'waitForEvent', name });
    return { payload: 'data', timestamp: '2024-01-01' };
  }
}

// Without workflows_step_rollback, step is the raw RPC stub.
// All basic step methods still work without wrapping.
export const noRollbackWithoutFlag = {
  async test(_, env) {
    const mock = new MockStep();
    const result = await env.BasicWorkflow.run({ payload: 'test' }, mock);

    assert.strictEqual(result.doResult, 'hello');
    assert.strictEqual(result.slept, true);
    assert.strictEqual(result.waited, true);
    assert.strictEqual(mock.calls.length, 4);
  },
};
