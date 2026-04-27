// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { RpcTarget } from 'cloudflare:workers';
import assert from 'node:assert';

// Mock step that records calls for verification.
class MockStep extends RpcTarget {
  calls = [];

  async do(...args) {
    const name = args[0];
    let callback,
      rollbackFn = null,
      rollbackConfig = null;
    if (typeof args[1] === 'function') {
      callback = args[1];
      rollbackFn = args[2] || null;
      rollbackConfig = args[3] || null;
    } else {
      callback = args[2];
      rollbackFn = args[3] || null;
      rollbackConfig = args[4] || null;
    }
    this.calls.push({
      method: 'do',
      name,
      hadRollbackFn: rollbackFn !== null,
      hadRollbackConfig: rollbackConfig !== null,
      rollbackConfig,
    });
    return await callback();
  }

  async sleep(name, duration) {
    this.calls.push({ method: 'sleep', name, duration });
  }

  async sleepUntil(name, timestamp) {
    this.calls.push({ method: 'sleepUntil', name, timestamp });
  }

  async waitForEvent(...args) {
    this.calls.push({
      method: 'waitForEvent',
      name: args[0],
      hadRollbackFn: (args[2] || null) !== null,
    });
    return {
      payload: 'event-data',
      timestamp: '2024-01-01',
      type: args[1]?.type,
    };
  }
}

// Mock step that invokes the rollback fn, simulating engine compensation.
// Captures the args passed to the rollback fn so the test can verify them.
class RollbackCallingMockStep extends RpcTarget {
  rollbackArgs = null;

  async do(...args) {
    const name = args[0];
    const callback = typeof args[1] === 'function' ? args[1] : args[2];
    const rollbackFn =
      typeof args[1] === 'function' ? args[2] || null : args[3] || null;
    const result = await callback();
    if (rollbackFn !== null) {
      const ctx = {
        error: 'simulated-error',
        output: result,
        stepName: name,
      };
      await rollbackFn(ctx);
      this.rollbackArgs = ctx;
    }
    return result;
  }

  async sleep() {}
  async sleepUntil() {}
  async waitForEvent(...args) {
    return {
      payload: 'event-data',
      timestamp: '2024-01-01',
      type: args[1]?.type,
    };
  }
}

// Verifies all forwarding paths: do (no rollback), do+rollback(fn),
// do+rollback(config,fn), sleep, sleepUntil, waitForEvent+rollback.
export const forwarding = {
  async test(_, env) {
    const mock = new MockStep();
    const result = await env.ForwardingWorkflow.run({ payload: 'test' }, mock);

    assert.strictEqual(result.basic, 'basic-result');
    assert.strictEqual(result.withFn, 'fn-result');
    assert.strictEqual(result.withConfig, 'config-result');
    assert.strictEqual(result.waited.type, 'approval');

    // 6 calls total: 3x do, sleep, sleepUntil, waitForEvent
    assert.strictEqual(mock.calls.length, 6);

    // do('basic') — no rollback args
    assert.strictEqual(mock.calls[0].hadRollbackFn, false);

    // do('with-fn') — rollback fn, no config
    assert.strictEqual(mock.calls[1].hadRollbackFn, true);
    assert.strictEqual(mock.calls[1].hadRollbackConfig, false);

    // do('with-config') — rollback fn + config
    assert.strictEqual(mock.calls[2].hadRollbackFn, true);
    assert.strictEqual(mock.calls[2].hadRollbackConfig, true);
    assert.deepStrictEqual(mock.calls[2].rollbackConfig, {
      retries: { limit: 3, delay: '5 seconds', backoff: 'linear' },
      timeout: '30 seconds',
    });

    // sleep / sleepUntil pass through
    assert.strictEqual(mock.calls[3].method, 'sleep');
    assert.strictEqual(mock.calls[4].method, 'sleepUntil');

    // waitForEvent with rollback
    assert.strictEqual(mock.calls[5].method, 'waitForEvent');
    assert.strictEqual(mock.calls[5].hadRollbackFn, true);
  },
};

// .rollback() guards: called twice, after await, with invalid arg
export const errorGuards = {
  async test(_, env) {
    const mock = new MockStep();
    const result = await env.ErrorGuardsWorkflow.run({ payload: 'test' }, mock);

    assert.strictEqual(result.errors.length, 3);
    assert.match(result.errors[0], /can only be called once/);
    assert.match(result.errors[1], /must be called before the step is awaited/);
    assert.match(result.errors[2], /expects a function/);
  },
};

// Step callback error surfaces through StepPromise
export const errorPropagation = {
  async test(_, env) {
    const mock = new MockStep();
    const result = await env.ErrorPropagationWorkflow.run(
      { payload: 'test' },
      mock
    );
    assert.strictEqual(result.threw, true);
    assert.strictEqual(result.message, 'step-boom');
  },
};

// Rollback fn is callable over RPC with correct context
export const rollbackCallable = {
  async test(_, env) {
    const mock = new RollbackCallingMockStep();
    const result = await env.RollbackCallableWorkflow.run(
      { payload: 'test' },
      mock
    );
    assert.strictEqual(result.value, 'step-output');
    assert.strictEqual(mock.rollbackArgs.output, 'step-output');
    assert.strictEqual(mock.rollbackArgs.stepName, 'my-step');
    assert.strictEqual(mock.rollbackArgs.error, 'simulated-error');
  },
};
