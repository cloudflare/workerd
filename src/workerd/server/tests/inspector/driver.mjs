import { env } from 'node:process';
import { beforeEach, afterEach, test } from 'node:test';
import assert from 'node:assert';
import CDP from 'chrome-remote-interface';
import { WorkerdServerHarness } from '@workerd/test/server-harness.mjs';

// Global that is reset for each test.
let workerd;

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WORKERD_CONFIG !== undefined,
  'You must set the WORKERD_CONFIG environment variable.'
);

// Start workerd.
beforeEach(async () => {
  workerd = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: env.WORKERD_CONFIG,

    // Hard-coded to match a socket name expected in the `workerdConfig` file.
    listenPortNames: ['http'],
  });

  await workerd.start();

  // We wait for the worker's HTTP port to come online before starting the test case. If we don't,
  // and the inspector port comes online first, there's a chance the inspector connection will fail
  // with 404 because the isolate doesn't exist yet.
  await workerd.getListenPort('http');
});

// Stop workerd.
afterEach(async () => {
  const [code, signal] = await workerd.stop();
  assert(code === 0 || signal === 'SIGTERM');
  workerd = null;
});

async function connectInspector(port) {
  return await CDP({
    port,

    // Hard-coded to match a service name expected in the `workerdConfig` file.
    target: '/main',

    // Required to avoid trying to load the Protocol (schema, I guess?) from workerd, which doesn't
    // implement the inspector protocol message in question.
    local: true,
  });
}

async function profileAndExpectDeriveBitsFrames(inspectorClient) {
  // Enable and start profiling.
  await inspectorClient.Profiler.enable();
  await inspectorClient.Profiler.start();

  // Drive the worker with a test request. A single one is sufficient.
  let httpPort = await workerd.getListenPort('http');
  const response = await fetch(`http://localhost:${httpPort}`);
  await response.arrayBuffer();

  // Stop and disable profiling.
  const profile = await inspectorClient.Profiler.stop();
  await inspectorClient.Profiler.disable();

  // Figure out which function name was most frequently sampled.
  let hitCountMap = new Map();

  for (let node of profile.profile.nodes) {
    if (hitCountMap.get(node.callFrame.functionName) === undefined) {
      hitCountMap.set(node.callFrame.functionName, 0);
    }
    hitCountMap.set(
      node.callFrame.functionName,
      hitCountMap.get(node.callFrame.functionName) + node.hitCount
    );
  }

  let max = {
    name: null,
    count: 0,
  };

  for (let [name, count] of hitCountMap) {
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

// Regression test for https://github.com/cloudflare/workerd/issues/1754.
//
// At one time, workerd produced only "(program)" frames.
test('Profiler mostly sees deriveBits() frames', async () => {
  let inspectorClient = await connectInspector(
    await workerd.getListenInspectorPort()
  );
  await profileAndExpectDeriveBitsFrames(inspectorClient);
  await inspectorClient.close();
});

// Regression test for https://github.com/cloudflare/workerd/issues/2564.
//
// At one time, workerd segfaulted on the second inspector connection.
test('Can repeatedly reconnect the inspector and profiling still works', async () => {
  for (let i = 0; i < 4; ++i) {
    let inspectorClient = await connectInspector(
      await workerd.getListenInspectorPort()
    );
    await profileAndExpectDeriveBitsFrames(inspectorClient);
    await inspectorClient.close();
  }
});
