// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests the experimental, local-dev-only real `node:inspector` implementation that connects to the
// isolate's own V8 inspector. Gated by the `enable_nodejs_inspector_local_dev` compat flag.

import inspector from 'node:inspector';
import inspectorPromises from 'node:inspector/promises';
import { Session } from 'node:inspector';
import { Session as PromiseSession } from 'node:inspector/promises';
import assert from 'node:assert';
import { EventEmitter } from 'node:events';

// A function we can run to generate some precise-coverage data.
function add(a, b) {
  if (a > b) {
    return a + b;
  }
  return b - a;
}

export const sessionIsConstructible = {
  test() {
    // With the experimental flag enabled, Session must be constructible (the stub throws).
    const session = new Session();
    assert.ok(session instanceof EventEmitter);
    session.disconnect(); // no-op when not connected
  },
};

export const callbackPostWorks = {
  async test() {
    const session = new Session();
    session.connect();

    // Exercise the callback-based post() API (node:inspector, non-promises form), including the
    // "params omitted, callback is the second argument" overload. Profiler.enable resolves with an
    // empty result object and, unlike Runtime.evaluate, does not require a default execution
    // context (which workerd does not mark).
    const result = await new Promise((resolve, reject) => {
      session.post('Profiler.enable', (err, params) => {
        if (err) reject(err);
        else resolve(params);
      });
    });
    assert.strictEqual(typeof result, 'object');

    // And the three-argument form (method, params, callback).
    await new Promise((resolve, reject) => {
      session.post('Profiler.setSamplingInterval', { interval: 100 }, (err) => {
        if (err) reject(err);
        else resolve(undefined);
      });
    });

    session.disconnect();
  },
};

export const postBeforeConnectThrows = {
  test() {
    const session = new Session();
    assert.throws(() => session.post('Runtime.evaluate', { expression: '1' }), {
      code: 'ERR_INSPECTOR_NOT_CONNECTED',
    });
  },
};

export const doubleConnectThrows = {
  test() {
    const session = new Session();
    session.connect();
    assert.throws(() => session.connect(), {
      code: 'ERR_INSPECTOR_ALREADY_CONNECTED',
    });
    session.disconnect();
  },
};

export const preciseCoverageWorks = {
  async test() {
    const session = new PromiseSession();
    session.connect();

    await session.post('Profiler.enable');
    await session.post('Profiler.startPreciseCoverage', {
      callCount: true,
      detailed: true,
    });

    // Exercise some code so there is coverage to collect.
    let total = 0;
    for (let i = 0; i < 5; i++) {
      total += add(i, i + 1);
    }
    assert.ok(total > 0);

    const coverage = await session.post('Profiler.takePreciseCoverage');
    assert.ok(Array.isArray(coverage.result));
    // Every script-coverage entry must have the v8-to-istanbul shape vitest depends on.
    for (const scriptCoverage of coverage.result) {
      assert.strictEqual(typeof scriptCoverage.scriptId, 'string');
      assert.strictEqual(typeof scriptCoverage.url, 'string');
      assert.ok(Array.isArray(scriptCoverage.functions));
      for (const fn of scriptCoverage.functions) {
        assert.strictEqual(typeof fn.functionName, 'string');
        assert.ok(Array.isArray(fn.ranges));
        for (const range of fn.ranges) {
          assert.strictEqual(typeof range.startOffset, 'number');
          assert.strictEqual(typeof range.endOffset, 'number');
          assert.strictEqual(typeof range.count, 'number');
        }
      }
    }

    await session.post('Profiler.stopPreciseCoverage');
    await session.post('Profiler.disable');
    session.disconnect();
  },
};

export const notificationsAreEmitted = {
  async test() {
    const session = new PromiseSession();
    session.connect();

    const scriptParsed = new Promise((resolve) => {
      session.once('Debugger.scriptParsed', (msg) => {
        resolve(msg);
      });
    });

    // Debugger.enable triggers Debugger.scriptParsed notifications for already-parsed scripts.
    await session.post('Debugger.enable');

    const msg = await scriptParsed;
    assert.strictEqual(msg.method, 'Debugger.scriptParsed');
    assert.ok(msg.params);

    await session.post('Debugger.disable');
    session.disconnect();
  },
};

export const defaultExportsArePresent = {
  test() {
    assert.strictEqual(typeof inspector.Session, 'function');
    assert.strictEqual(typeof inspectorPromises.Session, 'function');
    // The flag only enables the real Session. The process-level inspector functions on
    // node:inspector/promises (url/waitForDebugger/open/close) are not needed by the coverage use
    // case and remain unimplemented stubs that throw, regardless of the flag (matching master).
    assert.throws(() => inspectorPromises.url(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    assert.throws(() => inspectorPromises.close(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};
