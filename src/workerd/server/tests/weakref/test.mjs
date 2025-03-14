/*
This is a node.js script which handlers workerd lifecycle and sends multiple requests.
FinalizationRegistry callbacks run after the exported handler returns, and wd-test,
which only run tests within a single test() handler, are not enough to test this behaviour
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

// FinalizationRegistry callbacks only run after the exported handler returns
// So we should see its effects in a follow-up request
test('JS FinalizationRegistry', async () => {
  let httpPort = await workerd.getListenPort('http');
  for (let i = 0; i < 3; ++i) {
    const response = await fetch(`http://localhost:${httpPort}`);
    assert.strictEqual(await response.text(), `${i}`);
  }
});
