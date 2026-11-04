// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `workerd run` end to end: source on stdin and from --eval, bundled with esbuild, tested
// (--test) and served (--port 0, the port read off the `Listening on` line), with a template.
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { createServer } from 'node:net';
import { dirname, join, resolve } from 'node:path';
import { arch, env, platform } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

assert(env.WORKERD_BINARY !== undefined, 'WORKERD_BINARY must be set.');
assert(env.RUN_TEST_DIR !== undefined, 'RUN_TEST_DIR must be set.');

const WORKERD = resolve(env.WORKERD_BINARY);
// The working directory of every run: the source's imports resolve from it.
const DIR = resolve(env.RUN_TEST_DIR);

// The native esbuild of this platform, from the platform package beside the esbuild package.
// The esbuild package's own bin/esbuild may be a Node script, which workerd cannot run.
function esbuild() {
  const fromEsbuild = createRequire(
    createRequire(import.meta.url).resolve('esbuild/package.json')
  );
  const pkg = dirname(
    fromEsbuild.resolve(`@esbuild/${platform}-${arch}/package.json`)
  );
  return platform === 'win32'
    ? join(pkg, 'esbuild.exe')
    : join(pkg, 'bin', 'esbuild');
}
const ESBUILD = esbuild();

// Runs `workerd run <args>` to completion with `stdin`.
function run(args, stdin = '') {
  return new Promise((resolve) => {
    let output = '';
    const child = spawn(WORKERD, ['run', ...args], {
      cwd: DIR,
      env: { ...env, WORKERD_ESBUILD: ESBUILD },
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    child.stdout.on('data', (data) => (output += data));
    child.stderr.on('data', (data) => (output += data));
    child.on('close', (code) => resolve({ output, code }));
    child.stdin.end(stdin);
  });
}

const TESTED = `
import { greeting } from './greeting.ts';
export default {
  async test(): Promise<void> {
    const actual: string = greeting('test');
    if (actual !== 'Hello, test!') throw new Error('greeting is ' + actual);
  },
};
`;

test('a here-document on stdin is bundled as TypeScript and tested', async () => {
  const { output, code } = await run(['-', '--test'], TESTED);
  assert.strictEqual(code, 0, output);
  assert.match(output, /\[ PASS \] main/, output);
});

test('a failing test fails the run', async () => {
  const { output, code } = await run([
    '--test',
    '--eval',
    'export default { test() { throw new Error("boom") } }',
  ]);
  assert.notStrictEqual(code, 0, output);
  assert.match(output, /\[ FAIL \] main/, output);
  assert.match(output, /boom/, output);
});

test('a template gives the Worker its bindings', async () => {
  const { output, code } = await run([
    '--test',
    '--config=template.capnp',
    '--eval',
    `export default {
      test(ctrl, env) {
        if (env.GREETING !== 'hi from the template') throw new Error('GREETING is ' + env.GREETING);
      },
    }`,
  ]);
  assert.strictEqual(code, 0, output);
  assert.match(output, /\[ PASS \] app/, output);
});

test('an unknown autogate is a usage error', async () => {
  const { output, code } = await run([
    '--test',
    '--autogate=no-such-gate',
    '--eval',
    'export default {}',
  ]);
  assert.strictEqual(code, 1, output);
  assert.match(output, /No such autogate/, output);
});

// Starts `workerd run <args>` and waits for its `Listening on` line; returns the URL it gives and
// a function that stops the server.
async function serve(args) {
  const child = spawn(WORKERD, ['run', ...args], {
    cwd: DIR,
    env: { ...env, WORKERD_ESBUILD: ESBUILD },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  child.stderr.on('data', (data) => (output += data));
  const closed = new Promise((resolve) => child.on('close', resolve));
  const stop = async () => {
    child.kill();
    await closed;
  };
  try {
    const url = await new Promise((resolve, reject) => {
      child.stdout.on('data', (data) => {
        output += data;
        const match = output.match(/^Listening on (http:\/\/localhost:\d+)$/m);
        if (match) resolve(match[1]);
      });
      child.on('close', (code) =>
        reject(new Error(`exited with ${code}: ${output}`))
      );
    });
    return { url, stop, output: () => output };
  } catch (e) {
    await stop();
    throw e;
  }
}

const FETCHED = `import { greeting } from './greeting.ts';
export default { fetch: () => new Response(greeting('fetch')) };`;

test('port 0 listens on a free port, which the Listening line gives', async () => {
  const { url, stop } = await serve(['--port=0', '--eval', FETCHED]);
  try {
    assert.doesNotMatch(url, /:0$/);
    const response = await fetch(url);
    assert.strictEqual(await response.text(), 'Hello, fetch!');
  } finally {
    await stop();
  }
});

// Listens on `port` on every loopback address it can, so that nothing else can.
async function hold(port) {
  const servers = [];
  for (const host of ['127.0.0.1', '::1']) {
    const server = createServer();
    const listening = await new Promise((resolve) => {
      server.once('error', () => resolve(false));
      server.listen(port, host, () => resolve(true));
    });
    if (listening) servers.push(server);
  }
  return () => Promise.all(servers.map((s) => new Promise((r) => s.close(r))));
}

test('a taken default port moves to the next free one', async () => {
  // 8787 may be taken by something else already, which is as good.
  const release = await hold(8787);
  try {
    const { url, stop } = await serve(['--eval', FETCHED]);
    try {
      const port = Number(new URL(url).port);
      assert.ok(port > 8787 && port < 8787 + 10, url);
      const response = await fetch(url);
      assert.strictEqual(await response.text(), 'Hello, fetch!');
    } finally {
      await stop();
    }
  } finally {
    await release();
  }
});

test('a taken port given with --port is an error', async () => {
  const release = await hold(8787);
  try {
    const { output, code } = await run(['--port=8787', '--eval', FETCHED]);
    assert.notStrictEqual(code, 0, output);
    assert.match(output, /Socket "http"/, output);
  } finally {
    await release();
  }
});
