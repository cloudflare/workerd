// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { spawn } from 'node:child_process';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env, platform } from 'node:process';
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

test('--v8-flag passes a flag to V8, with or without `=`', async () => {
  for (const args of [
    ['--v8-flag=--expose-gc'],
    ['--v8-flag', '--expose-gc'],
  ]) {
    const { code, results, stderr } = await repl(args, ['typeof gc']);
    assert.strictEqual(code, 0, stderr);
    assert.deepStrictEqual(results, ["'function'"]);
  }
});

test('--v8-flag refuses a flag V8 does not know', async () => {
  const { code, stderr } = await repl(['--v8-flag=--no-such-v8-flag'], []);
  assert.strictEqual(code, 1, stderr);
  assert(stderr.includes('unrecognized V8 flag'), stderr);
  assert(stderr.includes('--no-such-v8-flag'), stderr);
});

test('--autogate enables an autogate', async () => {
  const input = [
    "(await import('workerd:unsafe')).default.isTestAutogateEnabled()",
  ];
  for (const [args, enabled] of [
    [['--autogate=test-workerd'], 'true'],
    [[], 'false'],
  ]) {
    const { code, results, stderr } = await repl(
      ['--compat-flag=unsafe_module', ...args],
      input
    );
    assert.strictEqual(code, 0, stderr);
    assert.deepStrictEqual(results, [enabled]);
  }
});

test('--autogate refuses a name that is not an autogate', async () => {
  const { code, stderr } = await repl(['--autogate=no-such-gate'], []);
  assert.strictEqual(code, 1, stderr);
  assert(stderr.includes('No such autogate: no-such-gate.'), stderr);
  assert(stderr.includes('test-workerd'), stderr);
});

// Starts `workerd repl --experimental ...args` for a test to give input to as it goes.
function startRepl(args) {
  const child = spawn(env.WORKERD_BINARY, ['repl', '--experimental', ...args], {
    stdio: ['pipe', 'pipe', 'pipe'],
    env: { ...env, WORKERD_REPL_HISTORY: '' },
  });
  const output = { stdout: '', stderr: '' };
  let waiting = [];
  const check = () => {
    waiting = waiting.filter(
      ({ until, resolve }) => !(until(output) && (resolve(), true))
    );
  };
  child.stdout.on('data', (data) => {
    output.stdout += data;
    check();
  });
  child.stderr.on('data', (data) => {
    output.stderr += data;
    check();
  });
  const exited = new Promise((resolve, reject) => {
    child.on('error', reject);
    child.on('close', (code, signal) => resolve({ code, signal }));
  });
  return {
    output,
    // Resolves to the exit status and the signal that ended the REPL, if one did.
    exited,
    send(line) {
      child.stdin.write(`${line}\n`);
    },
    // Presses Ctrl+C, as the terminal would signal it.
    interrupt() {
      child.kill('SIGINT');
    },
    // Resolves once `until(output)` holds.
    waitFor(until) {
      return new Promise((resolve) => {
        waiting.push({ until, resolve });
        check();
      });
    },
    // Ends the input; the exit status.
    async end() {
      child.stdin.end();
      return (await exited).code;
    },
  };
}

// A config whose worker answers every fetch with `text`.
function configAnswering(text) {
  return `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule = "export default { fetch() { return new Response('${text}'); } };" ),
        ],
      ),
    ),
  ],
);
`;
}

test('a watched config reloads when its file changes', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'repl-watch-'));
  const config = join(dir, 'config.capnp');
  await writeFile(config, configAnswering('one'));
  const fetchMain = "await (await s.service('main').fetch('http://x/')).text()";

  const repl = startRepl([]);
  repl.send(
    `s = await workerd.loadConfig(${JSON.stringify(config)}, { watch: true })`
  );
  repl.send(fetchMain);
  await repl.waitFor(({ stdout }) => stdout.includes("'one'"));

  await writeFile(config, configAnswering('two'));
  await repl.waitFor(({ stderr }) => stderr.includes('Reloaded '));
  repl.send(fetchMain);
  await repl.waitFor(({ stdout }) => stdout.includes("'two'"));

  // A change that breaks the config leaves the server running.
  await writeFile(config, 'not a config');
  await repl.waitFor(({ stderr }) => stderr.includes('did not reload'));
  repl.send(fetchMain);
  await repl.waitFor(({ stdout }) => stdout.split("'two'").length === 3);

  assert.strictEqual(await repl.end(), 0, repl.output.stderr);
});

const unixOnly = {
  skip: platform === 'win32' && 'Ctrl+C abandons an input only on Unix',
};

test('Ctrl+C stops awaiting an input', unixOnly, async () => {
  const repl = startRepl([]);
  repl.send("console.log('awaiting'); await new Promise(() => {})");
  await repl.waitFor(({ stdout }) => stdout.includes('awaiting'));
  repl.interrupt();
  await repl.waitFor(({ stderr }) => stderr.includes('Interrupted: '));
  repl.send('1 + 1');
  await repl.waitFor(({ stdout }) => stdout.endsWith('2\n'));
  assert.strictEqual(await repl.end(), 0, repl.output.stderr);
});

test(
  'a second Ctrl+C ends the REPL while an input runs without awaiting',
  unixOnly,
  async () => {
    const repl = startRepl([]);
    repl.send("console.log('spinning'); while (true) {}");
    await repl.waitFor(({ stdout }) => stdout.includes('spinning'));
    // Signals that arrive together are taken as one, so this signals until the REPL ends.
    const interrupting = setInterval(() => repl.interrupt(), 10);
    const exit = await repl.exited;
    clearInterval(interrupting);
    assert.deepStrictEqual(exit, { code: null, signal: 'SIGINT' });
  }
);
