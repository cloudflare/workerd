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

export const reentrantPostsAreDeliveredInOrder = {
  async test() {
    const session = new Session();
    session.connect();

    // post() callbacks run while the connection delivers messages, so a post() from a callback
    // dispatches reentrantly. Its response is delivered after the current callback returns.
    // Session reports exceptions thrown by callbacks instead of propagating them, so failures
    // are forwarded to reject() explicitly.
    const order = [];
    await new Promise((resolve, reject) => {
      const guard =
        (fn) =>
        (...args) => {
          try {
            fn(...args);
          } catch (e) {
            reject(e);
          }
        };
      session.post(
        'Profiler.enable',
        guard((err) => {
          assert.ifError(err);
          order.push('A:start');
          session.post(
            'Profiler.setSamplingInterval',
            { interval: 100 },
            guard((err) => {
              assert.ifError(err);
              order.push('B:start');
              session.post(
                'Profiler.disable',
                guard((err) => {
                  assert.ifError(err);
                  order.push('C');
                  resolve();
                })
              );
              order.push('B:end');
            })
          );
          order.push('A:end');
        })
      );
    });
    assert.deepStrictEqual(order, [
      'A:start',
      'A:end',
      'B:start',
      'B:end',
      'C',
    ]);
    session.disconnect();
  },
};

export const notificationsFromCallbacksAreDeliveredImmediately = {
  test() {
    const session = new Session();
    session.connect();
    const logged = [];
    session.on('Runtime.consoleAPICalled', (msg) => {
      logged.push(msg.params.args[0].value);
    });
    session.post('Runtime.enable', () => {
      console.log('from-callback');
    });
    // The notification is raised while the callback runs, and delivered before post() returns.
    assert.ok(logged.includes('from-callback'), JSON.stringify(logged));
    session.disconnect();
  },
};

export const reentryFromEvaluatedCodeIsHandled = {
  test() {
    const session = new Session();
    session.connect();
    let contextId;
    session.once('Runtime.executionContextCreated', (msg) => {
      contextId = msg.params.context.id;
    });
    session.post('Runtime.enable');
    assert.strictEqual(typeof contextId, 'number');

    // Code evaluated by the session's own V8 inspector calling back into the session cannot
    // dispatch, but may disconnect.
    let reentrantError;
    globalThis.reenterInspectorSession = () => {
      try {
        session.post('Runtime.enable');
      } catch (e) {
        reentrantError = e;
      }
      session.disconnect();
    };
    try {
      session.post('Runtime.evaluate', {
        expression: 'reenterInspectorSession()',
        contextId,
      });
    } finally {
      delete globalThis.reenterInspectorSession;
    }
    assert.match(String(reentrantError), /not connected/);
    assert.throws(() => session.post('Runtime.enable'), {
      code: 'ERR_INSPECTOR_NOT_CONNECTED',
    });
  },
};

export const throwingCallbacksAreReported = {
  test() {
    const session = new Session();
    session.connect();
    const reported = [];
    const onError = (event) => {
      reported.push(event.error);
      event.preventDefault();
    };
    globalThis.addEventListener('error', onError);
    try {
      const boom = new Error('boom');
      session.post('Profiler.enable', () => {
        throw boom;
      });
      assert.deepStrictEqual(reported, [boom]);

      // The session keeps working after a callback throws.
      let result;
      session.post('Profiler.disable', (err, params) => {
        result = { err, params };
      });
      assert.ifError(result.err);
      assert.strictEqual(typeof result.params, 'object');
    } finally {
      globalThis.removeEventListener('error', onError);
      session.disconnect();
    }
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
