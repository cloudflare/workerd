// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// The queues module (`/workerd/queues.js`) under `workerd serve --threads=4`:
// messages sent through every thread are each delivered once, and a queue's
// shard, a Durable Object, lives on whichever thread its home is, the first
// or another.
//
// A test learns the threads by the identities of their isolates: the app
// Worker answers with its isolate's, and refuses a request that names
// another (`x-thread`) so that the test can send it again until the isolate
// it names serves it. A shard delivers to the consumer of its own thread, so
// the isolate a consumer reports is that of the shard's home thread. Only the
// first thread serves the datagram socket, so the `probe` Worker behind it
// answers with the isolate of the first thread.

import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import dgram from 'node:dgram';
import { once } from 'node:events';
import { mkdtemp, mkdir, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { DatabaseSync } from 'node:sqlite';
import { test } from 'node:test';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';

const THREADS = 4;
const UUID = /^[0-9a-f-]{36}$/;
// The queues the config has, each with its own shard, and so its own home.
const QUEUES = Array.from({ length: 8 }, (_, i) => `q${i}`);

// `/where?ms=` answers this isolate after `ms`; `/send?q=&bodies=&delay=`
// sends the JSON array `bodies` to the queue binding `q` and answers its
// shard's metrics, as `/metrics?q=` does. `queue()` logs each message it
// takes with the isolate that took it.
const WORKER = `let isolate;
const isolateId = () => (isolate ??= crypto.randomUUID());
const attempts = new Set();
export default {
  async fetch(request, env) {
    const front = isolateId();
    const wanted = request.headers.get('x-thread');
    const attempt = request.headers.get('x-attempt');
    if (wanted !== null && (wanted !== front || attempts.has(attempt))) {
      await scheduler.wait(100);
      return Response.json({ front }, { status: 409 });
    }
    if (attempt !== null) attempts.add(attempt);
    const url = new URL(request.url);
    const query = url.searchParams;
    const queue = env[query.get('q')];
    if (url.pathname === '/where') {
      await scheduler.wait(Number(query.get('ms') ?? 0));
      return Response.json({ front });
    }
    if (url.pathname === '/send') {
      const delaySeconds = Number(query.get('delay') ?? 0);
      const bodies = JSON.parse(query.get('bodies'));
      await queue.sendBatch(
        bodies.map((body) => ({ body, contentType: 'text', delaySeconds }))
      );
    }
    return Response.json({ front, metrics: await queue.metrics() });
  },
  async queue(batch) {
    for (const m of batch.messages) {
      console.log('DELIVERED ' + m.body + ' ' + isolateId());
    }
  },
};
`;

// Answers each datagram with what the app Worker of its thread, the first,
// answers \`/where\`.
const PROBE = `export default {
  async connect(socket, env) {
    const writer = socket.writable.getWriter();
    for await (const _ of socket.readable) {
      const where = await env.APP.fetch('http://app/where');
      await writer.write(new Datagram(new Uint8Array(await where.arrayBuffer())));
    }
  },
};
`;

const json = (value) => JSON.stringify(JSON.stringify(value));

const config = (dir) => `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    ( name = "store", disk = ( path = ${JSON.stringify(join(dir, 'store'))}, writable = true ) ),
    ( name = "app",
      worker = (
        modules = [ ( name = "app.js", esModule = embed "app.js" ) ],
        compatibilityDate = "2026-01-01",
        bindings = [
          ${QUEUES.map(
            (q) =>
              `( name = "${q}", queue = ( name = "queues", props = ( json = ${json({ queue: q })} ) ) ),`
          ).join('\n          ')}
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
          ( name = "CONSUMERS", json = ${json(
            Object.fromEntries(
              QUEUES.map((q) => [q, { consumer: 'app', maxBatchTimeout: 0 }])
            )
          )} ),
          ( name = "app", service = "app" ),
        ],
        durableObjectNamespaces = [
          ( className = "QueueShard", uniqueKey = "QueueShard", enableSql = true ),
        ],
        durableObjectStorage = ( localDisk = "store" ),
      )
    ),
    ( name = "probe",
      worker = (
        modules = [ ( name = "probe.js", esModule = embed "probe.js" ) ],
        compatibilityDate = "2026-01-01",
        compatibilityFlags = [ "experimental" ],
        bindings = [ ( name = "APP", service = "app" ) ],
      )
    ),
  ],
  sockets = [
    ( name = "http", address = "127.0.0.1:0", http = (), service = "app" ),
    ( name = "udp", address = "127.0.0.1:0", udp = ( idleTimeoutMs = 200 ), service = "probe" ),
  ],
);
`;

// Runs workerd on `dir`'s config with `threads` threads. `delivered` holds
// each delivery, `{ body, isolate }`; `until(f, ms)` resolves once f() holds,
// checked as output arrives.
async function serve(dir, threads) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'config.capnp'),
      '--experimental',
      `--threads=${threads}`,
      '--control-fd=3',
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe'] }
  );
  const server = { threads, ports: {}, delivered: [], log: '' };
  const waiters = new Set();
  const read = (stream) => {
    let line = '';
    stream.setEncoding('utf8').on('data', (d) => {
      process.stderr.write(d);
      server.log += d;
      const lines = (line + d).split('\n');
      line = lines.pop();
      for (const l of lines) {
        const m = l.match(/DELIVERED (\S+) (\S+)/);
        if (m) server.delivered.push({ body: m[1], isolate: m[2] });
      }
      for (const check of waiters) check();
    });
  };
  read(child.stdout);
  read(child.stderr);
  const exited = new Promise((resolve) =>
    child.once('close', (code, signal) => resolve([code, signal]))
  );
  server.until = (f, ms, what) =>
    new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        waiters.delete(check);
        reject(new Error(`timed out waiting for ${what}`));
      }, ms);
      const check = () => {
        if (!f()) return;
        clearTimeout(timer);
        waiters.delete(check);
        resolve();
      };
      waiters.add(check);
      check();
    });
  server.stop = () => (child.kill('SIGTERM'), exited);
  server.kill = () => (child.kill('SIGKILL'), exited);

  let control = '';
  child.stdio[3].setEncoding('utf8');
  for await (const chunk of child.stdio[3]) {
    control += chunk;
    for (const l of control.split('\n').slice(0, -1)) {
      const event = JSON.parse(l);
      if (event.event === 'listen') server.ports[event.socket] = event.port;
    }
    if ('http' in server.ports && 'udp' in server.ports) break;
  }
  assert('http' in server.ports, `the server did not listen: ${server.log}`);
  return server;
}

// Sends `path` until the isolate `thread` serves it, or once to any isolate
// if `thread` is undefined, and answers the parsed response. A round sends
// several attempts a few milliseconds apart, twice as many each round, which
// an isolate other than the one named holds for a while and refuses: the
// front gives the next attempt to another thread. The attempts share an
// `x-attempt`, so the isolate named acts on one of them only.
async function get(server, path, thread) {
  const once = async (headers) => {
    const r = await fetch(`http://127.0.0.1:${server.ports.http}${path}`, {
      headers,
    });
    const text = await r.text();
    if (r.status === 409 && thread !== undefined) return undefined;
    assert.strictEqual(r.status, 200, `${path}: ${text}`);
    const answer = JSON.parse(text);
    if (thread !== undefined) assert.strictEqual(answer.front, thread);
    return answer;
  };
  if (thread === undefined) return once({});
  const headers = { 'x-thread': thread, 'x-attempt': randomUUID() };
  for (let round = 0; round < 8; round++) {
    const attempts = [];
    for (let i = 0; i < server.threads * 2 ** round; i++) {
      attempts.push(once(headers));
      await scheduler.wait(2);
    }
    const answers = (await Promise.all(attempts)).filter(
      (a) => a !== undefined
    );
    assert(answers.length <= 1, `isolate ${thread} acted on ${answers.length}`);
    if (answers.length === 1) return answers[0];
  }
  assert.fail(`isolate ${thread} served none of 8 rounds of ${path}`);
}

// The isolates of the app Worker, one per thread: a round sends a request per
// thread at once, each answered after a while, so that a thread with one in
// flight is passed over for one with none.
async function threadsOf(server) {
  const fronts = new Set();
  for (let round = 0; round < 8 && fronts.size < server.threads; round++) {
    const answers = [];
    for (let i = 0; i < server.threads; i++) {
      answers.push(get(server, '/where?ms=250'));
      await scheduler.wait(2);
    }
    for (const { front } of await Promise.all(answers)) fronts.add(front);
  }
  assert.strictEqual(fronts.size, server.threads, 'isolates that served');
  return [...fronts];
}

// The isolate of the first thread, which alone serves the datagram socket.
async function firstThread(server) {
  const socket = dgram.createSocket('udp4');
  try {
    const greeting = once(socket, 'message');
    for (let tries = 0; ; tries++) {
      socket.send('hello', server.ports.udp, '127.0.0.1');
      const answered = await Promise.race([
        greeting,
        scheduler.wait(500).then(() => undefined),
      ]);
      if (answered !== undefined) {
        const { front } = JSON.parse(answered[0].toString());
        assert.match(front, UUID);
        return front;
      }
      assert(tries < 20, 'the datagram socket did not answer');
    }
  } finally {
    socket.close();
  }
}

// The alarms each alarm database of the shards' namespace holds, by file:
// `metadata.sqlite` is the first thread's, `metadata-<n>.sqlite` thread n's.
async function alarms(dir) {
  const namespace = join(dir, 'store', 'QueueShard');
  const files = (await readdir(namespace)).filter((f) =>
    /^metadata(-\d+)?\.sqlite$/.test(f)
  );
  return Object.fromEntries(
    files.sort().map((file) => {
      const db = new DatabaseSync(join(namespace, file), { readOnly: true });
      try {
        return [
          file,
          db.prepare('SELECT count(*) AS n FROM _cf_ALARM').get().n,
        ];
      } finally {
        db.close();
      }
    })
  );
}

async function project() {
  const dir = await mkdtemp(
    join(env.TEST_TMPDIR ?? tmpdir(), 'workerd-queues-')
  );
  await mkdir(join(dir, 'store'));
  await writeFile(join(dir, 'app.js'), WORKER);
  await writeFile(join(dir, 'probe.js'), PROBE);
  await writeFile(join(dir, 'config.capnp'), config(dir));
  return dir;
}

// Where the shard of each queue in `queues` lives: the isolate of its
// consumer, which takes a message sent to it through any thread.
async function shardHomes(server, queues) {
  const homes = new Map();
  for (const q of queues) {
    const body = `${q}:where`;
    await get(
      server,
      `/send?q=${q}&bodies=${encodeURIComponent(JSON.stringify([body]))}`
    );
    await server.until(
      () => server.delivered.some((d) => d.body === body),
      10_000,
      `${body} to be delivered`
    );
    homes.set(q, server.delivered.find((d) => d.body === body).isolate);
  }
  return homes;
}

test('messages sent through every thread are each delivered once, by the shard on its home thread', async () => {
  const dir = await project();
  const server = await serve(dir, THREADS);
  try {
    const fronts = await threadsOf(server);
    const first = await firstThread(server);
    assert(fronts.includes(first), `${first} is not one of ${fronts}`);

    // Each queue's shard lives on one thread, which delivers its messages; at
    // least one of them is a thread but the first.
    const homes = await shardHomes(server, QUEUES);
    for (const home of homes.values()) assert(fronts.includes(home), home);
    const away = QUEUES.find((q) => homes.get(q) !== first);
    assert(
      away !== undefined,
      `the shards of all of ${QUEUES} are on the first thread`
    );
    const home = QUEUES.find((q) => homes.get(q) === first);

    // Through each thread, messages to the queue away from the first thread,
    // and to one on it if there is one.
    const queues = home === undefined ? [away] : [away, home];
    const sent = [];
    for (const [t, front] of fronts.entries()) {
      for (const q of queues) {
        const bodies = Array.from({ length: 5 }, (_, i) => `${q}:t${t}-${i}`);
        await get(
          server,
          `/send?q=${q}&bodies=${encodeURIComponent(JSON.stringify(bodies))}`,
          front
        );
        sent.push(...bodies);
      }
    }
    const ours = () => server.delivered.filter((d) => sent.includes(d.body));
    await server.until(
      () => ours().length >= sent.length,
      15_000,
      'every message'
    );
    // Time for a second delivery of any of them.
    await scheduler.wait(1000);
    assert.deepStrictEqual(
      ours()
        .map((d) => d.body)
        .sort(),
      [...sent].sort()
    );
    for (const { body, isolate } of ours()) {
      assert.strictEqual(isolate, homes.get(body.split(':')[0]), body);
    }

    // A message due in an hour: its shard's alarm is set, on its home thread,
    // in that thread's database, and every thread sees it in the backlog.
    await get(
      server,
      `/send?q=${away}&delay=3600&bodies=${encodeURIComponent(JSON.stringify([`${away}:later`]))}`
    );
    for (const front of fronts) {
      const { metrics } = await get(server, `/metrics?q=${away}`, front);
      assert.strictEqual(metrics.backlogCount, 1, front);
    }
    const held = await alarms(dir);
    const others = Object.entries(held).filter(
      ([file, n]) => file !== 'metadata.sqlite' && n > 0
    );
    assert(
      others.length > 0,
      `no thread but the first holds an alarm: ${JSON.stringify(held)}`
    );
  } catch (error) {
    await server.kill();
    throw error;
  } finally {
    await server.stop();
    await rm(dir, { recursive: true });
  }
  assert.doesNotMatch(
    server.log,
    /a service leaked|fatal runtime error|internal bug/
  );
});
