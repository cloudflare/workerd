// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { test } from 'node:test';
import assert from 'node:assert';
import { connectInspector, workerd } from './inspector-test-harness.mjs';

async function profileAndExpectDeriveBitsFrames(inspectorClient) {
  // Enable and start profiling.
  await inspectorClient.Profiler.enable();
  await inspectorClient.Profiler.start();

  // Drive the worker with a test request. A single one is sufficient.
  const httpPort = await workerd.getListenPort('http');
  const response = await fetch(`http://localhost:${httpPort}/pbkdf2Derive`);
  await response.arrayBuffer();

  // Stop and disable profiling.
  const profile = await inspectorClient.Profiler.stop();
  await inspectorClient.Profiler.disable();

  // Figure out which function name was most frequently sampled.
  const hitCountMap = new Map();

  for (const node of profile.profile.nodes) {
    if (hitCountMap.get(node.callFrame.functionName) === undefined) {
      hitCountMap.set(node.callFrame.functionName, 0);
    }
    hitCountMap.set(
      node.callFrame.functionName,
      hitCountMap.get(node.callFrame.functionName) + node.hitCount
    );
  }

  const max = {
    name: null,
    count: 0,
  };

  for (const [name, count] of hitCountMap) {
    if (count > max.count) {
      max.name = name;
      max.count = count;
    }
  }

  // The most CPU-intensive function our test script runs is `deriveBits()`, so we expect that to be
  // the most frequently sampled function.
  assert.equal(max.name, 'deriveBits');
  assert.notEqual(max.count, 0);
}

// Regression test for:
// - https://github.com/cloudflare/workerd/issues/1754
// - https://github.com/cloudflare/workerd/issues/2564
//
// At one time, workerd profiling broke, and started producing only "(program)" frames. My original
// attempt at a fix subsequently caused workerd to segfault on the second inspector connection. This
// rather expensive test case exercises both regressions.
test('Profiler mostly sees deriveBits() frames, and can safely reconnect', async () => {
  for (let i = 0; i < 2; ++i) {
    const inspectorClient = await connectInspector(
      await workerd.getListenInspectorPort()
    );
    await profileAndExpectDeriveBitsFrames(inspectorClient);
    await inspectorClient.close();
  }
});
