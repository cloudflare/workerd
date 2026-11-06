// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { spawn } from 'node:child_process';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WD_TEST_CONFIG !== undefined,
  'You must set the WD_TEST_CONFIG environment variable.'
);
assert(
  env.WD_THROWS_CONFIG !== undefined,
  'You must set the WD_THROWS_CONFIG environment variable.'
);

// Runs `workerd test` on a config and returns its stdout and stderr, interleaved.
function runWorkerd(config, extraArgs) {
  return new Promise((resolve) => {
    let output = '';
    const child = spawn(
      env.WORKERD_BINARY,
      ['test', config, '--experimental', ...extraArgs],
      { stdio: ['pipe', 'pipe', 'pipe'] }
    );
    child.stdout.on('data', (data) => (output += data));
    child.stderr.on('data', (data) => (output += data));
    child.on('close', (code) => resolve({ code, output }));
  });
}

const MARKS = [
  'MARK used log',
  'MARK used warn',
  'MARK fallback log',
  'MARK fallback warn',
];

// The zygote and the Worker that starts without a snapshot both evaluate the top level, but
// only one of them may print it.
function assertEachMarkOnce(output) {
  for (const mark of MARKS) {
    const count = output.split(mark).length - 1;
    assert.strictEqual(
      count,
      1,
      `"${mark}" printed ${count} times:\n${output}`
    );
  }
}

// `workerd test` logs at INFO, so a Worker that fell back to starting without a snapshot says so.
const SKIPPED = 'startup snapshot skipped';

test('top-level console output is printed once, snapshot used or not', async () => {
  const { code, output } = await runWorkerd(env.WD_TEST_CONFIG, []);
  assert.strictEqual(code, 0, output);
  assertEachMarkOnce(output);
  const skipped = output.split(SKIPPED).length - 1;
  assert.strictEqual(skipped, 1, `expected one fallback:\n${output}`);
  // The skip is logged for the `fallback` service, not for `used`.
  const skippedLine = output.split('\n').find((line) => line.includes(SKIPPED));
  assert(skippedLine?.includes('= fallback'), output);
});

test('with an inspector configured, the zygote does not run', async () => {
  const { code, output } = await runWorkerd(env.WD_TEST_CONFIG, [
    '--inspector-addr=127.0.0.1:0',
  ]);
  assert.strictEqual(code, 0, output);
  assertEachMarkOnce(output);
  assert(
    !output.includes(SKIPPED),
    `zygote ran under an inspector:\n${output}`
  );
});

test('a top level that throws gets no snapshot and reports its error once', async () => {
  const { code, output } = await runWorkerd(env.WD_THROWS_CONFIG, []);
  // The Worker's own evaluation fails, so the config is refused.
  assert.notStrictEqual(code, 0, output);
  // The zygote fails before anything is recorded and says why.
  const skipped = output.split(SKIPPED).length - 1;
  assert.strictEqual(skipped, 1, `expected one fallback:\n${output}`);
  assert(output.includes('the zygote Worker threw'), output);
  assert(output.includes("the zygote's top level failed"), output);
  // The zygote's console line is dropped with it; the Worker's evaluation prints it.
  const logged = output.split('MARK throws log').length - 1;
  assert.strictEqual(
    logged,
    1,
    `"MARK throws log" printed ${logged} times:\n${output}`
  );
  // The Worker's failure is the one reported.
  assert(output.includes('Uncaught Error: MARK throws error'), output);
});
