// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { env } from 'node:process';
import { beforeEach, afterEach } from 'node:test';
import assert from 'node:assert';
import CDP from 'chrome-remote-interface';
import { WorkerdServerHarness } from '../server-harness.mjs';

export let workerd;

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WORKERD_CONFIG !== undefined,
  'You must set the WORKERD_CONFIG environment variable.'
);

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

afterEach(async () => {
  const [code, signal] = await workerd.stop();
  assert(code === 0 || signal === 'SIGTERM');
  workerd = null;
});

export async function connectInspector(port) {
  return await CDP({
    port,

    // Hard-coded to match a service name expected in the `workerdConfig` file.
    target: '/main',

    // Required to avoid trying to load the Protocol (schema, I guess?) from workerd, which doesn't
    // implement the inspector protocol message in question.
    local: true,
  });
}
