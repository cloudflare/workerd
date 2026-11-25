// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// The Workflows engine (`/workerd/workflows.js`) under `workerd serve
// --threads=4`: instances created through every thread run to completion,
// each on its home thread, and every thread reports their status and output;
// not every instance lives on the first thread.
//
// A test learns the threads by the identities of their isolates: the app
// Worker answers with its isolate's, and refuses a request that names
// another (`x-thread`) so that the test can send it again until the isolate
// it names serves it. An instance is a Durable Object, and its engine runs
// the Workflow's `run()` in the app Worker of its own thread, so the isolate
// a step reports is that of the instance's home thread. Only the first
// thread serves the datagram socket, so the `probe` Worker behind it answers
// with the isolate of the first thread.

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
// Instances created through each thread.
const PER_THREAD = 3;

// `/where?ms=` answers this isolate after `ms`; `/create?id=&params=`
// creates an instance; `/status?id=` answers its status. `run()` takes a
// step, sleeps a second (an alarm of the instance's), and takes another, each
// step answering the isolate it ran in.
const WORKER = `import { WorkflowEntrypoint } from 'cloudflare:workers';
let isolate;
const isolateId = () => (isolate ??= crypto.randomUUID());
const attempts = new Set();
export class Flow extends WorkflowEntrypoint {
  async run(event, step) {
    const before = await step.do('before', async () => isolateId());
    await step.sleep('nap', '1 second');
    const after = await step.do('after', async () => isolateId());
    return { params: event.payload, before, after };
  }
}
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
    const id = query.get('id');
    if (url.pathname === '/where') {
      await scheduler.wait(Number(query.get('ms') ?? 0));
      return Response.json({ front });
    }
    if (url.pathname === '/create') {
      const params = query.get('params');
      const instance = await env.FLOW.create({ id, params });
      return Response.json({ front, id: instance.id });
    }
    const instance = await env.FLOW.get(id);
    return Response.json({ front, ...(await instance.status()) });
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

const flow = `( name = "workflows", entrypoint = "WorkflowBinding",
  props = ( json = "{\\"workflow\\": \\"flow\\"}" ) )`;

const config = (dir) => `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    ( name = "store",
      disk = ( path = ${JSON.stringify(join(dir, 'store'))}, writable = true ) ),
    ( name = "app",
      worker = (
        modules = [ ( name = "app.js", esModule = embed "app.js" ) ],
        compatibilityDate = "2026-01-01",
        bindings = [
          ( name = "FLOW", wrapped = (
              moduleName = "cloudflare-internal:workflows-api",
              innerBindings = [ ( name = "fetcher", service = ${flow} ) ] ) ),
        ],
        workflowsEngine = (
          actorClass = ( name = "workflows", entrypoint = "Engine" ),
          workflows = [
            ( className = "Flow", name = "flow", bindingService = ${flow} ) ],
        ),
      )
    ),
    ( name = "workflows",
      worker = (
        modules = [
          ( name = "workflows.js", bundled = "/workerd/workflows.js" ) ],
        compatibilityDate = "2026-01-01",
        bindings = [
          ( name = "workflow:flow", durableObjectNamespace = (
              className = "miniflare-workflows-flow", serviceName = "app" ) ),
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

// Runs workerd on `dir`'s config with `threads` threads.
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
  const server = { threads, ports: {}, log: '' };
  for (const stream of [child.stdout, child.stderr]) {
    stream.setEncoding('utf8').on('data', (d) => {
      process.stderr.write(d);
      server.log += d;
    });
  }
  const exited = new Promise((resolve) =>
    child.once('close', (code, signal) => resolve([code, signal]))
  );
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

// The alarms each alarm database of the instances' namespace holds, by file:
// `metadata.sqlite` is the first thread's, `metadata-<n>.sqlite` thread n's.
async function alarms(dir) {
  const namespace = join(dir, 'store', 'miniflare-workflows-flow');
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

test('instances created through every thread complete on their home threads, and every thread reports them', async () => {
  const dir = await mkdtemp(
    join(env.TEST_TMPDIR ?? tmpdir(), 'workerd-workflows-')
  );
  await mkdir(join(dir, 'store'));
  await writeFile(join(dir, 'app.js'), WORKER);
  await writeFile(join(dir, 'probe.js'), PROBE);
  await writeFile(join(dir, 'config.capnp'), config(dir));
  const server = await serve(dir, THREADS);
  try {
    const fronts = await threadsOf(server);
    const first = await firstThread(server);
    assert(fronts.includes(first), `${first} is not one of ${fronts}`);

    const ids = [];
    for (const [t, front] of fronts.entries()) {
      for (let j = 0; j < PER_THREAD; j++) {
        const id = `flow-${t}-${j}`;
        const created = await get(
          server,
          `/create?id=${id}&params=p-${id}`,
          front
        );
        assert.strictEqual(created.id, id);
        ids.push(id);
      }
    }

    // Every instance completes, whichever thread is asked.
    const outputs = new Map();
    for (const id of ids) {
      for (let tries = 0; ; tries++) {
        const { status, output, error } = await get(server, `/status?id=${id}`);
        assert.strictEqual(
          error ?? null,
          null,
          `${id}: ${JSON.stringify(error)}`
        );
        if (status === 'complete') {
          outputs.set(id, output);
          break;
        }
        assert(tries < 300, `${id} is still ${status}`);
        await scheduler.wait(100);
      }
    }
    for (const id of ids) {
      const output = outputs.get(id);
      assert.strictEqual(output.params, `p-${id}`);
      // Both steps ran in one isolate, a thread's, across the sleep.
      assert(fronts.includes(output.before), `${id} ran in ${output.before}`);
      assert.strictEqual(output.after, output.before, id);
      for (const front of fronts) {
        const status = await get(server, `/status?id=${id}`, front);
        assert.strictEqual(status.status, 'complete', `${id} through ${front}`);
        assert.deepStrictEqual(status.output, output, `${id} through ${front}`);
      }
    }

    // Not every instance lives on the first thread.
    const away = ids.filter((id) => outputs.get(id).before !== first);
    assert(away.length > 0, `all of ${ids} run on the first thread`);
    // A finished instance's alarm, for the end of its retention, is in the
    // database of its home thread.
    const held = await alarms(dir);
    const others = Object.entries(held).filter(
      ([file, n]) => file !== 'metadata.sqlite' && n > 0
    );
    assert(
      others.length > 0,
      `no thread but the first holds an alarm: ${JSON.stringify(held)}`
    );
    const total = Object.values(held).reduce((a, b) => a + b, 0);
    assert.strictEqual(total, ids.length, JSON.stringify(held));
    assert.strictEqual(held['metadata.sqlite'] ?? 0, ids.length - away.length);
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
