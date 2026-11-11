// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { spawn } from 'node:child_process';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

assert(env.WORKERD_BINARY !== undefined, 'Set WORKERD_BINARY.');
assert(env.REPL_CONFIG !== undefined, 'Set REPL_CONFIG.');

// Runs `workerd repl --experimental ...args` with `lines` as its input; its exit status, what it
// wrote to stdout, where the results go, as text and as lines, and what it wrote to stderr. A
// result can take several lines, as an object or an exception's stack does.
function repl(args, lines) {
  return new Promise((resolve, reject) => {
    const child = spawn(
      env.WORKERD_BINARY,
      ['repl', '--experimental', ...args],
      {
        stdio: ['pipe', 'pipe', 'pipe'],
        env: { ...env, WORKERD_REPL_HISTORY: '' },
      }
    );
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (data) => (stdout += data));
    child.stderr.on('data', (data) => (stderr += data));
    child.on('error', reject);
    child.on('close', (code) =>
      resolve({
        code,
        stdout,
        results: stdout.split('\n').slice(0, -1),
        stderr,
      })
    );
    child.stdin.end(lines.map((line) => `${line}\n`).join(''));
  });
}

test('a config named on the command line is loaded into `s`', async () => {
  const { code, stdout, results, stderr } = await repl(
    [env.REPL_CONFIG],
    ["await (await s.service('main').fetch('http://x/')).text()"]
  );
  assert.strictEqual(code, 0, stderr);
  assert(stdout.startsWith('ReplServer {'), stdout);
  assert.strictEqual(results.at(-1), "'hello'", stdout);
  // The load is shown as the input it is.
  assert(stderr.includes('> s = await workerd.loadConfig('), stderr);
});

test('the command line can name the config constant', async () => {
  const { code, stdout, results, stderr } = await repl(
    [env.REPL_CONFIG, 'config'],
    ['s.closed']
  );
  assert.strictEqual(code, 0, stderr);
  assert(stdout.startsWith('ReplServer {'), stdout);
  assert.strictEqual(results.at(-1), 'false', stdout);
});

test('a config that does not load leaves the REPL running', async () => {
  const { code, stdout, results, stderr } = await repl(
    ['no-such-config.capnp'],
    ['typeof s', '1 + 1']
  );
  assert.strictEqual(code, 0, stderr);
  assert(stdout.startsWith('Uncaught Error: no-such-config.capnp'), stdout);
  assert.deepStrictEqual(results.slice(-2), ["'undefined'", '2'], stdout);
});
