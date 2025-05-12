/*
This is a node.js script which handlers workerd lifecycle and sends multiple requests.
FinalizationRegistry callbacks run, if scheduled, across I/O boundaries, and wd-test,
which only run tests within a single test() handler, are not enough to test this behaviour.
This test now covers both FinalizationRegistry and WeakRef APIs.
*/

import { env } from 'node:process';
import { beforeEach, afterEach, test } from 'node:test';
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
  workerd = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: env.WORKERD_CONFIG,

    // Hard-coded to match a socket name expected in the `workerdConfig` file.
    listenPortNames: ['http'],
  });

  await workerd.start();

  await workerd.getListenPort('http');
});

// Stop workerd.
afterEach(async () => {
  const [code, signal] = await workerd.stop();
  assert(code === 0 || signal === 'SIGTERM', `code=${code}, signal=${signal}`);
  workerd = null;
});

// FinalizationRegistry callbacks run across I/O boundaries
test('JS FinalizationRegistry', async () => {
  let httpPort = await workerd.getListenPort('http');

  // The first request doesn't do any I/O so we won't notice the effects
  // of FinalizationRegistry cleanup callbacks as part of the response
  const response = await fetch(`http://localhost:${httpPort}?test=fr`);
  assert.strictEqual(await response.text(), '0');

  // Subsequent requests do I/O so we will immediately see
  // the effects of FinalizationRegistry cleanup callbacks
  for (let i = 0; i < 2; ++i) {
    const response = await fetch(`http://localhost:${httpPort}?test=fr`);
    assert.strictEqual(await response.text(), `${i + 2}`);
  }
});

// Test WeakRef behavior
test('JS WeakRef', async () => {
  let httpPort = await workerd.getListenPort('http');

  // Create a new object and a WeakRef to it
  let response = await fetch(
    `http://localhost:${httpPort}?test=weakref&create`
  );
  let data = await response.json();

  // Verify the object is created and accessible via the WeakRef
  assert.strictEqual(data.created, true);
  assert.strictEqual(data.value, "I'm alive!");

  // Check that the WeakRef is still valid
  response = await fetch(`http://localhost:${httpPort}?test=weakref`);
  data = await response.json();
  assert.strictEqual(data.isDereferenced, false);
  assert.strictEqual(data.value, "I'm alive!");

  // Force garbage collection and check if the WeakRef is dereferenced
  response = await fetch(`http://localhost:${httpPort}?test=weakref&gc`);
  data = await response.json();
  // Check if the reference is gone after GC
  assert.strictEqual(data.isDereferenced, true);
  assert.strictEqual(data.value, null);

  // Create a new object again to verify WeakRef can be reset
  response = await fetch(`http://localhost:${httpPort}?test=weakref&create`);
  data = await response.json();

  // The new object should be accessible
  assert.strictEqual(data.created, true);
  assert.strictEqual(data.value, "I'm alive!");
});
