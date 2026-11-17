// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of the cron module (`/workerd/cron.js`): armed at startup
// with no request or socket, a failed occurrence retried with the same
// scheduledTime and not dropped by a restart, an occurrence that ran not
// run again behind a clock stepped back, also by a schedule stored before
// it recorded that; a bad expression, or no --experimental, refused.

import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { DatabaseSync } from 'node:sqlite';
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

// Fails every attempt at one expression, and runs the others.
const FAILING = '0-59 * * * *';
const ONE_FAILS = `export default {
  async scheduled({ cron, scheduledTime: at }) {
    console.log('SCHEDULED ' + JSON.stringify({ cron, at }));
    if (cron === ${JSON.stringify(FAILING)}) throw new Error('it fails');
  },
};
`;

const config = (
  dir,
  expressions
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
          ( name = "CRONS", json = ${JSON.stringify(JSON.stringify({ app: expressions }))} ),
          ( name = "app", service = "app" ),
          ( name = "TEST_CLOCK", json = "true" ),
        ],
        durableObjectNamespaces = [
          ( className = "Schedule", uniqueKey = "Schedule", enableSql = true ),
        ],
        durableObjectStorage = ( localDisk = "store" ),
      )
    ),
  ],
  sockets = [ ( name = "http", address = "127.0.0.1:0", http = (), service = "cron" ) ],
);
`;

async function project(expressions = ['* * * * *'], worker = WORKER) {
  const dir = await mkdtemp(join(tmpdir(), 'workerd-cron-'));
  await mkdir(join(dir, 'store'));
  await writeFile(join(dir, 'app.js'), worker);
  await writeFile(join(dir, 'config.capnp'), config(dir, expressions));
  return dir;
}

// Deletes `key` from the schedule's storage, of a server that is not running.
async function forget(dir, key) {
  const files = await readdir(join(dir, 'store'), { recursive: true });
  const [file] = files.filter(
    (f) => f.endsWith('.sqlite') && !f.endsWith('metadata.sqlite')
  );
  const db = new DatabaseSync(join(dir, 'store', file));
  db.prepare('DELETE FROM _cf_KV WHERE key = ?').run(key);
  db.close();
}

// Runs workerd on `dir`'s config. `calls` holds each scheduled() call so far;
// `until(f, ms)` resolves once f() holds, checked as each call arrives;
// `clock(ms)` advances the cron Worker's clock and resolves to its time.
function serve(dir, args) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'config.capnp'),
      '--control-fd=3',
      '--socket-addr=http=127.0.0.1:0',
      ...args,
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe'] }
  );
  const port = new Promise((resolve) => {
    let control = '';
    child.stdio[3].setEncoding('utf8').on('data', (d) => {
      control += d;
      for (const l of control.split('\n')) {
        const m = l.match(/"event":"listen".*"port":(\d+)/);
        if (m) resolve(Number(m[1]));
      }
    });
  });
  const clock = async (ms) => {
    const url = `http://127.0.0.1:${await port}/clock?advance=${ms}`;
    return (await (await fetch(url, { method: 'POST' })).json()).now;
  };
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
    clock,
    exited,
    output: () => output,
    stop: (signal) => (child.kill(signal), exited),
  };
}

test('a failed occurrence is retried, and a restart does not drop it', async () => {
  const dir = await project();
  let server = serve(dir, ['--experimental']);
  try {
    // A second before the next minute, which then runs; a real minute may
    // run first, before the move.
    const now = await server.clock(0);
    const at = now - (now % 60_000) + 60_000;
    const runs = () => server.calls.filter((c) => c.at === at);
    await server.clock(at - 1000 - now);
    await server.until(() => runs().length >= 1, 10_000);
    const [first] = runs();
    assert.deepStrictEqual(first, { cron: '* * * * *', at, n: 1 });
    // Killed as the first attempt fails, before its retry: once it starts
    // again, the occurrence runs, whether as the stored retry or afresh.
    await server.stop('SIGKILL');
    server = serve(dir, ['--experimental']);
    await server.until(() => runs().some(({ n }) => n === 2), 30_000);
    assert.deepStrictEqual(runs(), [first, { ...first, n: 2 }]);
  } finally {
    await server.stop();
    await rm(dir, { recursive: true });
  }
});

test('a clock stepped back does not run an occurrence again', async () => {
  const dir = await project();
  const server = serve(dir, ['--experimental']);
  try {
    // A second before a minute at least two seconds away, so that the clock
    // moves forward; a real minute may run first.
    const now = await server.clock(0);
    let at = now - (now % 60_000) + 60_000;
    if (at - now < 2000) at += 60_000;
    const runs = () => server.calls.filter((c) => c.at === at);
    await server.clock(at - 1000 - now);
    // The first attempt fails, and its retry runs the occurrence.
    await server.until(() => runs().length >= 2, 15_000);
    // Back to before the occurrence, which the schedule does not arm again.
    await server.clock(at - 1000 - (await server.clock(0)));
    await assert.rejects(server.until(() => runs().length > 2, 3000));
    assert.deepStrictEqual(
      runs().map(({ n }) => n),
      [1, 2]
    );
  } finally {
    await server.stop();
    await rm(dir, { recursive: true });
  }
});

test('a schedule stored without its last occurrence keeps its retry', async () => {
  const dir = await project(['* * * * *', FAILING], ONE_FAILS);
  let server = serve(dir, ['--experimental']);
  try {
    const now = await server.clock(0);
    let at = now - (now % 60_000) + 60_000;
    if (at - now < 2000) at += 60_000;
    const runs = (cron) =>
      server.calls.filter((c) => c.at === at && c.cron === cron);
    await server.clock(at - 1000 - now);
    // Its third attempt leaves the failing expression a retry 8 s away,
    // stored once the run is over, which an arm waits out.
    await server.until(() => runs(FAILING).length >= 3, 20_000);
    await server.clock(0);
    await server.stop('SIGKILL');
    // As a schedule stored before `fired` was: the retry alone records the
    // occurrence.
    await forget(dir, 'fired');
    server = serve(dir, ['--experimental']);
    // Short of the occurrence again: the retry is kept, and the expression
    // that ran does not run again.
    await server.clock(at - 500 - (await server.clock(0)));
    await assert.rejects(
      server.until(() => runs('* * * * *').length > 0, 3000)
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
    const dir = await project([expression]);
    const server = serve(dir, args);
    try {
      assert.notStrictEqual(await server.exited, 0);
      assert.match(server.output(), error);
    } finally {
      await rm(dir, { recursive: true });
    }
  });
}
