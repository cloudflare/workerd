/*
This is a node.js script which handlers workerd lifecycle and sends multiple requests.
FinalizationRegistry callbacks run, if scheduled, across I/O boundaries, and wd-test,
which only run tests within a single test() handler, are not enough to test this behaviour.
This test now covers both FinalizationRegistry and WeakRef APIs.
*/

import { env } from 'node:process';
import { beforeEach, afterEach, test, after } from 'node:test';
import assert from 'node:assert';
import { WorkerdServerHarness } from '../server-harness.mjs';

// Global that is reset for each test.
let workerd;

assert.notStrictEqual(
  env.WORKERD_BINARY,
  undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert.notStrictEqual(
  env.WORKERD_CONFIG,
  undefined,
  'You must set the WORKERD_CONFIG environment variable.'
);

// Start workerd.
beforeEach(async () => {
  console.log('[TEST] Starting workerd process...');
  workerd = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: env.WORKERD_CONFIG,

    // Hard-coded to match a socket name expected in the `workerdConfig` file.
    listenPortNames: ['http'],
  });

  await workerd.start();

  await workerd.getListenPort('http');
  console.log('[TEST] Workerd process started successfully');
});

// Stop workerd.
afterEach(async () => {
  if (workerd) {
    console.log('[TEST] Stopping workerd process...');
    try {
      const [code, signal] = await workerd.stop();
      console.log(`[TEST] Workerd stopped with code=${code}, signal=${signal}`);
      assert(
        code === 0 || signal === 'SIGTERM',
        `code=${code}, signal=${signal}`
      );
    } catch (error) {
      console.error('[TEST] Error stopping workerd:', error);
      throw error;
    } finally {
      workerd = null;
    }
  }
  console.log('[TEST] Cleanup complete');
});

// FinalizationRegistry callbacks run across I/O boundaries
test('JS FinalizationRegistry', async () => {
  let httpPort = await workerd.getListenPort('http');

  // The first request doesn't do any I/O so we won't notice the effects
  // of FinalizationRegistry cleanup callbacks as part of the response
  const response = await fetch(`http://localhost:${httpPort}?test=fr`);
  console.log('[TEST] First FR request completed');
  assert.strictEqual(await response.text(), '0');

  // Subsequent requests do I/O so we will immediately see
  // the effects of FinalizationRegistry cleanup callbacks
  for (let i = 0; i < 2; ++i) {
    console.log(`[TEST] FR request ${i + 2} starting...`);
    const response = await fetch(`http://localhost:${httpPort}?test=fr`);
    console.log(`[TEST] FR request ${i + 2} completed`);
    assert.strictEqual(await response.text(), `${i + 2}`);
  }
});

// Test WeakRef behavior
test('JS WeakRef', async () => {
  let httpPort = await workerd.getListenPort('http');

  // Create a new object and a WeakRef to it
  console.log('[TEST] Creating WeakRef object...');
  let response = await fetch(
    `http://localhost:${httpPort}?test=weakref&create`
  );
  let data = await response.json();
  console.log('[TEST] WeakRef object created:', data);

  // Verify the object is created and accessible via the WeakRef
  assert.strictEqual(data.created, true);
  assert.strictEqual(data.value, "I'm alive!");

  // Check that the WeakRef is still valid
  console.log('[TEST] Checking WeakRef validity...');
  response = await fetch(`http://localhost:${httpPort}?test=weakref`);
  data = await response.json();
  console.log('[TEST] WeakRef validity check result:', data);
  assert.strictEqual(data.isDereferenced, false);
  assert.strictEqual(data.value, "I'm alive!");

  // Force garbage collection and check if the WeakRef is dereferenced
  console.log('[TEST] Forcing garbage collection...');
  response = await fetch(`http://localhost:${httpPort}?test=weakref&gc`);
  data = await response.json();
  console.log('[TEST] GC result:', data);
  // Check if the reference is gone after GC
  assert.strictEqual(data.isDereferenced, true);
  assert.strictEqual(data.value, null);

  // Create a new object again to verify WeakRef can be reset
  console.log('[TEST] Creating new WeakRef object after GC...');
  response = await fetch(`http://localhost:${httpPort}?test=weakref&create`);
  data = await response.json();
  console.log('[TEST] New WeakRef object created:', data);

  // The new object should be accessible
  assert.strictEqual(data.created, true);
  assert.strictEqual(data.value, "I'm alive!");
});

// Ensure clean exit
after(async () => {
  console.log('[TEST] All tests completed, ensuring clean exit...');
  // Give a moment for any cleanup to complete
  await new Promise((resolve) => setTimeout(resolve, 100));

  // Force exit to prevent hanging
  console.log('[TEST] Exiting process...');
  process.exit(0);
});

// Handle uncaught errors
process.on('uncaughtException', (error) => {
  console.error('[TEST] Uncaught exception:', error);
  process.exit(1);
});
