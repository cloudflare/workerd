// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { test } from 'node:test';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';
import { connectInspector, workerd } from './inspector-test-harness.mjs';

// Regression test for use-after-free when sending Unicode exception messages to inspector.
// Before the fix, this would cause memory corruption or crashes due to the scratch buffer
// being freed before the inspector finished reading from it.
test('Inspector correctly receives exceptions with Unicode characters', async () => {
  const inspectorClient = await connectInspector(
    await workerd.getListenInspectorPort()
  );

  // Collect exceptions reported to the inspector
  const exceptions = [];
  inspectorClient.on('Runtime.exceptionThrown', (params) => {
    exceptions.push(params);
  });
  await inspectorClient.Runtime.enable();

  // Make the worker throw an exception with non-ascii.
  const message = '💥 错误 오류 エラー Ошибка';
  const httpPort = await workerd.getListenPort('http');
  const url = new URL(`http://localhost:${httpPort}/throwException`);
  url.searchParams.set('message', message);
  const response = await fetch(url);
  assert.strictEqual(response.status, 500);

  // Wait to receive the exception events
  let iters = 0;
  while (exceptions.length < 2) {
    await scheduler.wait(50);
    iters += 1;
    if (iters > 50) {
      assert.fail('timed out waiting for exceptions');
    }
  }

  // We actually receive two records for the exception, one "uncaught in promise" and one
  // "uncaught in response".
  assert.strictEqual(exceptions.length, 2);

  const lastException = exceptions[exceptions.length - 1];
  assert.strictEqual(
    lastException.exceptionDetails.text,
    `Uncaught Error: ${message}`
  );

  await inspectorClient.Runtime.disable();
  await inspectorClient.close();
});

// DevTools async stacks continue across timers and microtasks, which V8 can't see on its own; the
// runtime reports them as async tasks.
test('Inspector async stacks continue across setTimeout and queueMicrotask', async () => {
  const inspectorClient = await connectInspector(
    await workerd.getListenInspectorPort()
  );
  await inspectorClient.Debugger.enable();
  await inspectorClient.Debugger.setAsyncCallStackDepth({ maxDepth: 32 });

  const httpPort = await workerd.getListenPort('http');
  const cases = [
    {
      path: '/asyncStack/timer',
      description: 'setTimeout',
      scheduler: 'scheduleTimer',
    },
    {
      path: '/asyncStack/microtask',
      description: 'queueMicrotask',
      scheduler: 'scheduleMicrotask',
    },
  ];
  for (const { path, description, scheduler } of cases) {
    const paused = new Promise((resolve) => {
      inspectorClient.once('Debugger.paused', resolve);
    });
    const response = fetch(`http://localhost:${httpPort}${path}`);
    const params = await paused;
    await inspectorClient.Debugger.resume();
    assert.strictEqual((await response).status, 200);

    const asyncStack = params.asyncStackTrace;
    assert(asyncStack, `${path}: paused with an async stack`);
    assert.strictEqual(asyncStack.description, description);
    assert(
      asyncStack.callFrames.some((frame) => frame.functionName === scheduler),
      `${path}: the async stack includes ${scheduler}: ${JSON.stringify(asyncStack.callFrames)}`
    );
  }

  await inspectorClient.Debugger.disable();
  await inspectorClient.close();
});
