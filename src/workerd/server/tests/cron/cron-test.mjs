// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of the cron module (`/workerd/cron.js`): armed at startup
// with no request or socket, each occurrence once, a failed one retried with
// the same scheduledTime; a bad expression, or no --experimental, refused.

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
    ( name = "store", disk = ( path = "${join(dir, 'store')}", writable = true ) ),
    ( name = "app",
      worker = (
        modules = [ ( name = "app.js", esModule = embed "app.js" ) ],
        compatibilityDate = "2026-01-01",
      )
    ),
    ( name = "cron",
      worker = (
        modules = [ ( name = "cron.js", builtin = "/workerd/cron.js" ) ],
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

// Runs workerd on `dir`'s config. `calls` holds each scheduled() call so far.
function serve(dir, args) {
  const child = spawn(
    env.WORKERD_BINARY,
    ['serve', join(dir, 'config.capnp'), ...args],
    { stdio: ['ignore', 'pipe', 'pipe'] }
  );
  const calls = [];
  let output = '';
  const read = (d) => {
    process.stderr.write(d);
    output += d;
    for (const m of output.matchAll(/SCHEDULED (\{.*\})/g)) {
      if (m.index >= calls.consumed) calls.push(JSON.parse(m[1]));
    }
    calls.consumed = output.length;
  };
  calls.consumed = 0;
  child.stdout.on('data', read);
  child.stderr.on('data', read);
  const exited = new Promise((resolve) => child.once('exit', resolve));
  return {
    calls,
    exited,
    output: () => output,
    stop: () => (child.kill(), exited),
  };
}

async function until(f, ms) {
  const deadline = Date.now() + ms;
  while (!f()) {
    assert(Date.now() < deadline, 'timed out');
    await new Promise((r) => setTimeout(r, 100));
  }
}

test('each occurrence runs once, a failure is retried', async () => {
  const dir = await project();
  const server = serve(dir, ['--experimental']);
  try {
    const { calls } = server;
    await until(() => calls.length >= 2, 75_000);
    const [first, retry] = calls;
    assert.strictEqual(first.cron, '* * * * *');
    assert.strictEqual(first.at % 60_000, 0);
    assert.deepStrictEqual(retry, { ...first, n: 2 });
    await until(() => calls.length >= 4, 75_000);
    assert.deepStrictEqual(
      calls.map(({ at, n }) => [at - first.at, n]),
      [
        [0, 1],
        [0, 2],
        [60_000, 1],
        [60_000, 2],
      ]
    );
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
