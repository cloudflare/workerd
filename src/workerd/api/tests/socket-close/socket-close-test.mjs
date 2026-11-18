// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { spawn } from 'node:child_process';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

assert(env.WORKERD_BINARY !== undefined, 'WORKERD_BINARY must be set.');
assert(env.WD_TEST_CONFIG !== undefined, 'WD_TEST_CONFIG must be set.');

test('closing a socket with a pipe close in flight logs no uncaught exception', async () => {
  const { output, code } = await new Promise((resolve) => {
    let output = '';
    const child = spawn(
      env.WORKERD_BINARY,
      ['test', env.WD_TEST_CONFIG, '--experimental', '--verbose'],
      { stdio: ['pipe', 'pipe', 'pipe'] }
    );
    child.stdout.on('data', (data) => (output += data));
    child.stderr.on('data', (data) => (output += data));
    child.on('close', (code) => resolve({ output, code }));
  });

  assert.strictEqual(code, 0, output);
  assert.match(output, /\[ PASS \] main:proxiedEchoThenClose/, output);
  assert.doesNotMatch(output, /uncaught exception/, output);
  assert.doesNotMatch(output, /undefined/, output);
});
