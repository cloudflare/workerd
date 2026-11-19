// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { spawn } from 'node:child_process';
import http from 'node:http';
import net from 'node:net';
import { env } from 'node:process';
import { test } from 'node:test';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';
import { WorkerdServerHarness } from '../server-harness.mjs';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WORKERD_CONFIG !== undefined,
  'You must set the WORKERD_CONFIG environment variable.'
);

const THREADS = 4;

// How the tests serve on several threads: `threads`, a loop of its own on every thread, or
// `units`, as many loops as threads on a work-stealing scheduler, each with its own copy of every
// Worker, which serves the same as `threads` does.
const MODE = env.WORKERD_THREADS_MODE ?? 'threads';
assert(
  MODE === 'threads' || MODE === 'units',
  `WORKERD_THREADS_MODE must be threads or units, not ${MODE}`
);
// What only `threads` mode tests: one thread, and the usage errors of `--threads`.
const THREADS_MODE_ONLY = {
  skip: MODE !== 'threads' && 'threads mode tests this',
};

// Node's SIGTERM is TerminateProcess on Windows: the server neither drains nor exits by itself
// there.
const SIGTERM_DRAINS = {
  skip: process.platform === 'win32' && 'SIGTERM does not drain on Windows',
};
// How the server ends on SIGTERM, as `[code, signal]`.
const SIGTERM_EXIT =
  process.platform === 'win32' ? [null, 'SIGTERM'] : [0, null];
// More than one thread is a usage error on Windows.
const SEVERAL_THREADS = {
  skip:
    process.platform === 'win32' &&
    'several threads are not supported on Windows',
};

// The options that serve on `n` threads in this mode.
function threadArgs(n) {
  return MODE === 'units'
    ? [`--threads=${n}`, `--loops=${n}`, `--isolates=${n}`]
    : [`--threads=${n}`];
}

// Starts `workerd serve` with `extraArgs` and returns the harness and the port it listens on.
async function serve(extraArgs) {
  const workerd = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: env.WORKERD_CONFIG,
    listenPortNames: ['http'],
    extraArgs,
    // Several threads do not support the inspector.
    inspector: false,
  });
  await workerd.start();
  return { workerd, port: await workerd.getListenPort('http') };
}

// A request on a connection of its own. The server answers once the request body ends:
// `response` is the promise for the parsed response, `finish()` ends the body.
function request(port, path = '/') {
  const agent = new http.Agent({ keepAlive: false });
  const req = http.request({
    agent,
    host: '127.0.0.1',
    port,
    path,
    method: 'POST',
  });
  const response = new Promise((resolve, reject) => {
    req.on('error', reject);
    req.on('response', (res) => {
      let body = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => (body += chunk));
      res.on('error', reject);
      res.on('end', () => {
        try {
          assert.strictEqual(res.statusCode, 200, body);
          resolve(JSON.parse(body));
        } catch (error) {
          reject(error);
        }
      });
    });
  });
  // Sends the head now, so that the server has the request while its body is open.
  req.flushHeaders();
  return { response, finish: () => req.end() };
}

// Opens `count` requests at once and holds them all open, so that no thread can have served
// one and moved on to the next, then finishes them; their responses.
async function concurrentRequests(port, count) {
  const requests = Array.from({ length: count }, () => request(port));
  await scheduler.wait(100);
  for (const { finish } of requests) finish();
  return await Promise.all(requests.map(({ response }) => response));
}

// A request answered while another keeps its isolate's thread busy was served by another
// thread; the two responses. The busy thread cannot accept a connection, so a request that
// arrives meanwhile and is answered before the busy one was accepted by another thread.
async function requestDuringSpin(port) {
  const spin = request(port, '/spin?ms=400');
  spin.finish();
  let spinDone = false;
  const spun = spin.response.then((ids) => {
    spinDone = true;
    return ids;
  });
  await scheduler.wait(100);
  const other = request(port);
  other.finish();
  const answered = await other.response;
  const overlapped = !spinDone;
  return { spun: await spun, answered, overlapped };
}

test(
  'every thread serves its own copy of the Workers',
  SEVERAL_THREADS,
  async () => {
    const { workerd, port } = await serve([
      '--experimental',
      ...threadArgs(THREADS),
    ]);
    try {
      // Which `back` each `front` reached, by the isolates' identities.
      const backOf = new Map();
      const record = ({ front, back }) => {
        assert.match(front, /^[0-9a-f-]{36}$/);
        assert.match(back, /^[0-9a-f-]{36}$/);
        assert.notStrictEqual(front, back);
        // A service binding reaches the Worker of the same thread: always the same one.
        assert.strictEqual(backOf.get(front) ?? back, back);
        backOf.set(front, back);
      };

      // Which thread accepts a connection is the kernel's choice, so try until several did,
      // rather than expect a distribution.
      let overlaps = 0;
      for (let attempt = 0; attempt < 20; attempt++) {
        for (const ids of await concurrentRequests(port, 64)) record(ids);
        const { spun, answered, overlapped } = await requestDuringSpin(port);
        record(spun);
        record(answered);
        if (overlapped) {
          // Two requests in two isolates at once: two threads.
          assert.notStrictEqual(spun.front, answered.front);
          overlaps++;
        }
        if (backOf.size > 1 && overlaps > 0) break;
      }
      assert(backOf.size > 1, 'only one thread served requests');
      assert(overlaps > 0, 'no request was served while a thread was busy');
      assert(backOf.size <= THREADS, `${backOf.size} isolates of one Worker`);
      // Every thread has its own `back`, too.
      assert.strictEqual(new Set(backOf.values()).size, backOf.size);
    } finally {
      const [code, signal] = await workerd.stop();
      assert.deepStrictEqual([code, signal], SIGTERM_EXIT);
    }
  }
);

test(
  'SIGTERM drains the requests in flight on every thread',
  SIGTERM_DRAINS,
  async () => {
    const { workerd, port } = await serve([
      '--experimental',
      ...threadArgs(THREADS),
    ]);
    const requests = Array.from({ length: 64 }, () => request(port));
    // The server has the requests, and waits for their bodies.
    await scheduler.wait(500);
    workerd.kill('SIGTERM');
    // A draining server accepts no connection, and still serves the ones it has.
    await scheduler.wait(500);
    for (const { finish } of requests) finish();
    const responses = await Promise.all(
      requests.map(({ response }) => response)
    );
    assert.strictEqual(responses.length, 64);
    assert.deepStrictEqual(await workerd.closed(), [0, null]);
    await assert.rejects(request(port).response);
    await workerd.stop();
  }
);

for (const extraArgs of [[], ['--threads=1']]) {
  test(
    `one thread serves alone: ${JSON.stringify(extraArgs)}`,
    THREADS_MODE_ONLY,
    async () => {
      // One thread needs no --experimental.
      const { workerd, port } = await serve(extraArgs);
      try {
        const fronts = new Set();
        for (let attempt = 0; attempt < 3; attempt++) {
          for (const { front } of await concurrentRequests(port, 32)) {
            fronts.add(front);
          }
          const { spun, answered, overlapped } = await requestDuringSpin(port);
          fronts.add(spun.front).add(answered.front);
          assert(!overlapped, 'a request was served while the thread was busy');
        }
        assert.strictEqual(fronts.size, 1);
      } finally {
        const [code, signal] = await workerd.stop();
        assert.deepStrictEqual([code, signal], SIGTERM_EXIT);
      }
    }
  );
}

test(
  'every thread accepts from an inherited socket',
  {
    // `--socket-fd` names a socket handle there, which Node.js cannot pass to a child, and
    // several threads are not supported there either.
    skip: process.platform === 'win32' && 'Node.js passes no socket on Windows',
  },
  async () => {
    // The server gets its listening socket from this process, as descriptor 4 (`--socket-fd`),
    // and writes its events to descriptor 3.
    const listening = net.createServer();
    await new Promise((resolve) => listening.listen(0, '127.0.0.1', resolve));
    const { port } = listening.address();
    const child = spawn(
      env.WORKERD_BINARY,
      [
        'serve',
        env.WORKERD_CONFIG,
        '--experimental',
        ...threadArgs(THREADS),
        '--control-fd=3',
        '--socket-fd=http=4',
      ],
      { stdio: ['ignore', 'inherit', 'inherit', 'pipe', listening._handle.fd] }
    );
    const closed = new Promise((resolve) => {
      child.on('close', (code, signal) => resolve([code, signal]));
    });
    try {
      // This process only lends the socket: it accepts nothing from it.
      await new Promise((resolve) => listening.close(resolve));
      const control = child.stdio[3];
      control.setEncoding('utf8');
      let events = '';
      for await (const chunk of control) {
        events += chunk;
        if (events.includes('\n')) break;
      }
      assert.deepStrictEqual(JSON.parse(events.split('\n')[0]), {
        event: 'listen',
        socket: 'http',
        port,
      });

      const fronts = new Set();
      for (let attempt = 0; attempt < 20 && fronts.size < 2; attempt++) {
        for (const { front } of await concurrentRequests(port, 64)) {
          fronts.add(front);
        }
        const { spun, answered, overlapped } = await requestDuringSpin(port);
        fronts.add(spun.front).add(answered.front);
        if (overlapped) assert.notStrictEqual(spun.front, answered.front);
      }
      assert(fronts.size > 1, 'only one thread served requests');
    } finally {
      child.kill('SIGTERM');
      assert.deepStrictEqual(await closed, [0, null]);
    }
  }
);

// Runs `workerd serve` with `args` to its end; its exit code and what it wrote to stderr.
async function serveToExit(args) {
  const child = spawn(env.WORKERD_BINARY, ['serve', ...args], {
    stdio: ['ignore', 'inherit', 'pipe'],
  });
  let stderr = '';
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (data) => (stderr += data));
  const [code, signal] = await new Promise((resolve) => {
    child.on('close', (code, signal) => resolve([code, signal]));
  });
  return { code, signal, stderr };
}

test(
  'options several threads do not support are usage errors',
  { skip: SEVERAL_THREADS.skip || THREADS_MODE_ONLY.skip },
  async () => {
    for (const [args, message] of [
      [['--threads=2'], /--threads: .*`--experimental`/],
      [
        ['--threads=2', '--experimental', '--inspector-addr=127.0.0.1:0'],
        /--threads: --inspector-addr is not supported with more than one thread\./,
      ],
    ]) {
      const { code, signal, stderr } = await serveToExit([
        env.WORKERD_CONFIG,
        ...args,
      ]);
      assert.deepStrictEqual([code, signal], [1, null], stderr);
      assert.match(stderr, message);
    }
  }
);

test(
  'several threads are a usage error on Windows',
  {
    skip:
      (process.platform !== 'win32' && 'only Windows refuses them') ||
      THREADS_MODE_ONLY.skip,
  },
  async () => {
    const { code, signal, stderr } = await serveToExit([
      env.WORKERD_CONFIG,
      '--experimental',
      '--threads=2',
    ]);
    assert.deepStrictEqual([code, signal], [1, null], stderr);
    assert.match(
      stderr,
      /--threads: Serving on more than one thread is not supported on Windows\./
    );
  }
);
