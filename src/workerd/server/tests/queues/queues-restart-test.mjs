// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of the queues module (`/workerd/queues.js`), killed and
// started again: a shard reopens with its unacked messages and without the
// acked ones.

import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

// `/?q=<binding>&send=<n>&tag=<t>&delay=<s>` sends n messages, `<t><i>`,
// and answers the shard's metrics; `/clock` goes to the queues Worker.
const WORKER = `export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/clock') return env.QUEUES.fetch(request);
    const q = env[url.searchParams.get('q')];
    const n = Number(url.searchParams.get('send') ?? 0);
    const tag = url.searchParams.get('tag') ?? 'm';
    const delaySeconds = Number(url.searchParams.get('delay') ?? 0);
    for (let i = 0; i < n; i += 100) {
      const k = Math.min(100, n - i);
      const m = (j) => ({ body: tag + (i + j), contentType: 'text', delaySeconds });
      await q.sendBatch(Array.from({ length: k }, (_, j) => m(j)));
    }
    return Response.json(await q.metrics());
  },
  async queue(batch) {
    for (const m of batch.messages) console.log('DELIVERED ' + m.body);
  },
};
`;

const queue = (q) =>
  `queue = ( name = "queues", props = ( json = "{\\"queue\\":\\"${q}\\"}" ) )`;

const config = (dir) => `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    ( name = "store", disk = ( path = ${JSON.stringify(join(dir, 'store'))}, writable = true ) ),
    ( name = "app",
      worker = (
        modules = [ ( name = "app.js", esModule = embed "app.js" ) ],
        compatibilityDate = "2026-01-01",
        bindings = [
          ( name = "JOBS", ${queue('jobs')} ),
          ( name = "IDLE", ${queue('idle')} ),
          ( name = "QUEUES", service = "queues" ),
        ],
      )
    ),
    ( name = "queues",
      worker = (
        modules = [ ( name = "queues.js", bundled = "/workerd/queues.js" ) ],
        compatibilityDate = "2026-01-01",
        compatibilityFlags = [ "service_binding_extra_handlers" ],
        bindings = [
          ( name = "SHARD", durableObjectNamespace = "QueueShard" ),
          ( name = "CONSUMERS",
            json = "{\\"jobs\\": {\\"consumer\\": \\"app\\", \\"maxBatchTimeout\\": 0}}" ),
          ( name = "app", service = "app" ),
          ( name = "TEST_CLOCK", json = "true" ),
        ],
        durableObjectNamespaces = [
          ( className = "QueueShard", uniqueKey = "QueueShard", enableSql = true ),
        ],
        durableObjectStorage = ( localDisk = "store" ),
      )
    ),
  ],
  sockets = [ ( name = "http", address = "127.0.0.1:0", http = (), service = "app" ) ],
);
`;

// Runs workerd on `dir`'s config. `delivered` holds each delivered body;
// `until(f, ms)` resolves once f() holds, checked as output arrives;
// `get(q, send, tag, delay)` sends to binding q and resolves to its shard's
// metrics; `clock(ms)` advances the queues Worker's clock.
function serve(dir) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'config.capnp'),
      '--experimental',
      '--control-fd=3',
      '--socket-addr=http=127.0.0.1:0',
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe'] }
  );
  const port = new Promise((resolve) => {
    let control = '';
    child.stdio[3].setEncoding('utf8').on('data', (d) => {
      control += d;
      const m = control.match(/"event":"listen".*"port":(\d+)/);
      if (m) resolve(Number(m[1]));
    });
  });
  const get = async (q, send = 0, tag = 'm', delay = 0) => {
    const query = `q=${q}&send=${send}&tag=${tag}&delay=${delay}`;
    return (await fetch(`http://127.0.0.1:${await port}/?${query}`)).json();
  };
  const clock = async (ms) => {
    const url = `http://127.0.0.1:${await port}/clock?advance=${ms}`;
    await fetch(url, { method: 'POST' });
  };
  const delivered = [];
  const waiters = new Set();
  const read = (stream) => {
    let line = '';
    stream.setEncoding('utf8').on('data', (d) => {
      process.stderr.write(d);
      const lines = (line + d).split('\n');
      line = lines.pop();
      for (const l of lines) {
        const m = l.match(/DELIVERED (\S+)/);
        if (m) delivered.push(m[1]);
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
    delivered,
    until,
    get,
    clock,
    stop: (signal) => (child.kill(signal), exited),
  };
}

test('a restart keeps unacked messages, with their bodies, and drops acked ones', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'workerd-queues-'));
  await mkdir(join(dir, 'store'));
  await writeFile(join(dir, 'app.js'), WORKER);
  await writeFile(join(dir, 'config.capnp'), config(dir));
  let server = serve(dir);
  try {
    await server.get('JOBS', 3);
    await server.until(() => server.delivered.length === 3, 10_000);
    // Due in a minute: delivered after the restart, bodies read back then.
    await server.get('JOBS', 2, 'late', 60);
    // Held across the restart, counted on reopen.
    assert.strictEqual((await server.get('IDLE', 300)).backlogCount, 300);
    await server.stop('SIGKILL');

    server = serve(dir);
    const idle = await server.get('IDLE');
    assert.strictEqual(idle.backlogCount, 300);
    assert.strictEqual(idle.backlogBytes, 300 * 2 + 290 + 200);
    // A minute on, the reopened shard delivers what it held with the new
    // message, and none of the acked ones again.
    await server.clock(60_000);
    await server.get('JOBS', 1, 'new');
    await server.until(() => server.delivered.length === 3, 10_000);
    assert.deepStrictEqual(server.delivered.sort(), ['late0', 'late1', 'new0']);
  } finally {
    await server.stop();
    await rm(dir, { recursive: true });
  }
});
