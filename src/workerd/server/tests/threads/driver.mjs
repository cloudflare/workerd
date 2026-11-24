// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Stateless Workers under `workerd serve --threads`: every thread serves its own copy of each
// Worker, and each request goes to whichever thread takes it, whatever connection it came on.
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

// How many requests a test sends for one isolate before it gives up.
const TRIES = 200;

// Sends a request until the isolate of `front` whose identity is `thread` serves it, or any
// isolate if `thread` is undefined: a request names the isolate it is for (`x-thread`), and any
// other refuses it before reading its body. `attempt(headers)` sends the request once with
// `headers` and resolves with what it got, or with undefined if another isolate refused it.
async function steer(thread, attempt) {
  const headers = thread === undefined ? {} : { 'x-thread': thread };
  for (let tries = 0; tries < TRIES; tries++) {
    const answer = await attempt(headers);
    if (answer !== undefined) return answer;
  }
  assert.fail(`isolate ${thread} served none of ${TRIES} requests`);
}

// A request on a connection of its own, served by the isolate `thread` (any if undefined). The
// server answers once the request body ends: `response` is the promise for the parsed response,
// `finish()` ends the body. A request another isolate refused is sent again, its body open.
function request(port, path = '/', thread) {
  let current;
  let finished = false;
  const response = steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request({
          agent: false,
          host: '127.0.0.1',
          port,
          path,
          method: 'POST',
          headers,
        });
        current = req;
        req.on('error', reject);
        req.on('response', (res) => {
          let body = '';
          res.setEncoding('utf8');
          res.on('data', (chunk) => (body += chunk));
          res.on('error', reject);
          res.on('end', () => {
            try {
              if (res.statusCode === 409 && thread !== undefined) {
                req.destroy();
                resolve(undefined);
                return;
              }
              assert.strictEqual(res.statusCode, 200, body);
              const ids = JSON.parse(body);
              if (thread !== undefined) assert.strictEqual(ids.front, thread);
              resolve(ids);
            } catch (error) {
              reject(error);
            }
          });
        });
        // Sends the head now, so that the server has the request while its body is open.
        req.flushHeaders();
        if (finished) req.end();
      })
  );
  return {
    response,
    finish: () => {
      finished = true;
      if (!current.destroyed) current.end();
    },
  };
}

// The parsed response to a request for `path` that ends at once, served by the isolate
// `thread` (any if undefined).
function get(port, path, thread) {
  const { response, finish } = request(port, path, thread);
  finish();
  return response;
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
// thread, since a busy thread takes no request; the two responses, and whether the second was
// answered before the first.
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

// Sends requests one after another over one keep-alive connection until `threads` isolates of
// `front` have served them; their responses, in order. Nothing ties a connection to a thread:
// each request goes to whichever thread takes it, and an idle server wakes its threads in turn.
async function oneConnection(port, threads = THREADS) {
  const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
  const sockets = new Set();
  const responses = [];
  const fronts = new Set();
  try {
    for (let tries = 0; tries < TRIES && fronts.size < threads; tries++) {
      const ids = await new Promise((resolve, reject) => {
        const req = http.request(
          { agent, host: '127.0.0.1', port, path: '/' },
          (res) => {
            sockets.add(res.socket);
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
          }
        );
        req.on('error', reject);
        req.end();
      });
      responses.push(ids);
      fronts.add(ids.front);
    }
  } finally {
    agent.destroy();
  }
  assert.strictEqual(sockets.size, 1, 'connections the requests went over');
  assert.strictEqual(
    fronts.size,
    threads,
    'isolates that served one connection'
  );
  return responses;
}

// How a server told to stop ends, as `[code, signal]`, once `closed` resolves. A server still
// running after `ms` is killed with `kill()`, and fails the test.
async function exited(closed, kill, ms = 15_000) {
  let timer;
  const late = new Promise((resolve) => {
    timer = setTimeout(resolve, ms, 'late');
  });
  const result = await Promise.race([closed, late]);
  clearTimeout(timer);
  if (result === 'late') {
    kill();
    await closed;
    assert.fail(`the server still ran ${ms} ms after it was told to stop`);
  }
  return result;
}

// Runs `body` with the harness of a server started with `extraArgs` and the port it listens
// on, then stops the server, which ends as SIGTERM ends it. A server whose test failed is
// killed.
async function withServer(extraArgs, body) {
  const { workerd, port } = await serve(extraArgs);
  try {
    await body(workerd, port);
  } catch (error) {
    workerd.kill('SIGKILL');
    await workerd.closed();
    throw error;
  }
  assert.deepStrictEqual(await workerd.stop(), SIGTERM_EXIT);
}

test('every thread serves its own copy of the Workers', SEVERAL_THREADS, () =>
  withServer(['--experimental', `--threads=${THREADS}`], async (_, port) => {
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

    // The requests of one connection are served by every thread.
    for (const ids of await oneConnection(port)) record(ids);
    assert.strictEqual(backOf.size, THREADS);

    // A busy thread takes nothing, so another serves what arrives meanwhile. Try until a
    // request did, rather than expect timing.
    let overlaps = 0;
    for (let attempt = 0; attempt < 20 && overlaps === 0; attempt++) {
      for (const ids of await concurrentRequests(port, 64)) record(ids);
      const { spun, answered, overlapped } = await requestDuringSpin(port);
      record(spun);
      record(answered);
      if (overlapped) {
        // Two requests in two isolates at once: two threads.
        assert.notStrictEqual(spun.front, answered.front);
        overlaps++;
      }
    }
    assert(overlaps > 0, 'no request was served while a thread was busy');
    assert.strictEqual(backOf.size, THREADS, 'isolates of one Worker');
    // Every thread has its own `back`, too.
    assert.strictEqual(new Set(backOf.values()).size, backOf.size);
  })
);

test(
  'SIGTERM drains the requests in flight on every thread',
  SIGTERM_DRAINS,
  async () => {
    const { workerd, port } = await serve([
      '--experimental',
      `--threads=${THREADS}`,
    ]);
    try {
      const fronts = [
        ...new Set((await oneConnection(port)).map((r) => r.front)),
      ];
      // Each thread has requests that wait for their bodies.
      const PER_THREAD = 16;
      const wanted = fronts.flatMap((front) =>
        Array.from({ length: PER_THREAD }, () => front)
      );
      const requests = wanted.map((front) => request(port, '/', front));
      for (const front of fronts) {
        for (let tries = 0; ; tries++) {
          const { waiting } = await get(port, '/waiting', front);
          if (waiting === PER_THREAD) break;
          assert(tries < TRIES, `${waiting} requests wait on a thread`);
          await scheduler.wait(10);
        }
      }
      workerd.kill('SIGTERM');
      // A draining server accepts no connection, and still serves the ones it has.
      await scheduler.wait(500);
      for (const { finish } of requests) finish();
      const responses = await Promise.all(
        requests.map(({ response }) => response)
      );
      assert.deepStrictEqual(
        responses.map((ids) => ids.front),
        wanted
      );
      assert.deepStrictEqual(
        await exited(workerd.closed(), () => workerd.kill('SIGKILL')),
        [0, null]
      );
      await assert.rejects(request(port).response);
    } catch (error) {
      workerd.kill('SIGKILL');
      await workerd.closed();
      throw error;
    }
    await workerd.stop();
  }
);

for (const extraArgs of [[], ['--threads=1']]) {
  test(`one thread serves alone: ${JSON.stringify(extraArgs)}`, () =>
    // One thread needs no --experimental.
    withServer(extraArgs, async (_, port) => {
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
    }));
}

test(
  'every thread serves the requests of an inherited socket',
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
        `--threads=${THREADS}`,
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

      await oneConnection(port);
    } catch (error) {
      child.kill('SIGKILL');
      await closed;
      throw error;
    }
    child.kill('SIGTERM');
    assert.deepStrictEqual(await exited(closed, () => child.kill('SIGKILL')), [
      0,
      null,
    ]);
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
  SEVERAL_THREADS,
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
  { skip: process.platform !== 'win32' && 'only Windows refuses them' },
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
