// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of the cron module (`/workerd/cron.js`): armed at startup
// with no request or socket, a failed occurrence retried with the same
// scheduledTime, a pending retry run after a restart; a bad expression, or
// no --experimental, refused.

import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

// Fails the first attempt at each occurrence.
const WORKER = `const attempts = new Map();
export default {
  async scheduled(controller) {
    const at = controller.scheduledTime;
    const n = (attempts.get(at) ?? 0) + 1;
    attempts.set(at, n);
    console.log('SCHEDULED ' + JSON.stringify({ cron: controller.cron, at, n }));
    if (n === 1) throw new Error('first attempt fails');
  },
};
`;

const config = (
  dir,
  expression
) => `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    ( name = "store", disk = ( path = ${JSON.stringify(join(dir, 'store'))}, writable = true ) ),
    ( name = "app",
      worker = (
        modules = [ ( name = "app.js", esModule = embed "app.js" ) ],
        compatibilityDate = "2026-01-01",
      )
    ),
    ( name = "cron",
      worker = (
        modules = [ ( name = "cron.js", bundled = "/workerd/cron.js" ) ],
        compatibilityDate = "2026-01-01",
        compatibilityFlags = [ "service_binding_extra_handlers" ],
        bindings = [
          ( name = "SCHEDULE", durableObjectNamespace = "Schedule" ),
          ( name = "CRONS", json = "{\\"app\\": [\\"${expression}\\"]}" ),
          ( name = "app", service = "app" ),
        ],
        durableObjectNamespaces = [
          ( className = "Schedule", uniqueKey = "Schedule", enableSql = true ),
        ],
        durableObjectStorage = ( localDisk = "store" ),
      )
    ),
  ],
);
`;

async function project(expression = '* * * * *') {
  const dir = await mkdtemp(join(tmpdir(), 'workerd-cron-'));
  await mkdir(join(dir, 'store'));
  await writeFile(join(dir, 'app.js'), WORKER);
  await writeFile(join(dir, 'config.capnp'), config(dir, expression));
  return dir;
}

// Runs workerd on `dir`'s config. `calls` holds each scheduled() call so far;
// `until(f, ms)` resolves once f() holds, checked as each call arrives.
function serve(dir, args) {
  const child = spawn(
    env.WORKERD_BINARY,
    ['serve', join(dir, 'config.capnp'), ...args],
    { stdio: ['ignore', 'pipe', 'pipe'] }
  );
  const calls = [];
  const waiters = new Set();
  let output = '';
  // Each stream is parsed by whole lines: a chunk can end mid-line.
  const read = (stream) => {
    let line = '';
    stream.setEncoding('utf8').on('data', (d) => {
      process.stderr.write(d);
      output += d;
      const lines = (line + d).split('\n');
      line = lines.pop();
      for (const l of lines) {
        const m = l.match(/SCHEDULED (\{.*\})/);
        if (m) calls.push(JSON.parse(m[1]));
      }
      for (const check of waiters) check();
    });
  };
  read(child.stdout);
  read(child.stderr);
  const exited = new Promise((resolve) => child.once('exit', resolve));
  const until = (f, ms) =>
    new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('timed out')), ms);
      const check = () => {
        if (!f()) return;
        clearTimeout(timer);
        waiters.delete(check);
        resolve();
      };
      waiters.add(check);
      check();
    });
  return {
    calls,
    until,
    exited,
    output: () => output,
    stop: (signal) => (child.kill(signal), exited),
  };
}

// The one wait for the clock: the first occurrence, within a minute.
test('an occurrence is retried with its scheduledTime, across a restart', async () => {
  const dir = await project();
  let server = serve(dir, ['--experimental']);
  try {
    await server.until(() => server.calls.length >= 1, 75_000);
    const [first] = server.calls;
    assert.deepStrictEqual(first, {
      cron: '* * * * *',
      at: first.at - (first.at % 60_000),
      n: 1,
    });
    // Killed as the first attempt fails, before its retry; the retry runs
    // once it starts again.
    await server.stop('SIGKILL');
    server = serve(dir, ['--experimental']);
    await server.until(() => server.calls.some(({ n }) => n === 2), 30_000);
    assert.deepStrictEqual(server.calls, [first, { ...first, n: 2 }]);
  } finally {
    await server.stop();
    await rm(dir, { recursive: true });
  }
});

for (const [name, expression, args, error] of [
  [
    'a bad expression fails the config',
    '0 0 * * 0',
    ['--experimental'],
    /cron trigger "0 0 \* \* 0": day of week field has 0 outside 1-7/,
  ],
  ['cron triggers need --experimental', '* * * * *', [], /experimental/],
]) {
  test(name, async () => {
    const dir = await project(expression);
    const server = serve(dir, args);
    try {
      assert.notStrictEqual(await server.exited, 0);
      assert.match(server.output(), error);
    } finally {
      await rm(dir, { recursive: true });
    }
  });
}
