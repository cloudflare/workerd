// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Stateless Workers under `workerd serve --threads`: every thread serves its own copy of each
// Worker, and each request goes to whichever thread takes it, whatever connection it came on.
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
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

// How many requests a test sends before it gives up on what it waits for.
const TRIES = 200;

// What the `attempts` of a round resolved to, once each has settled. An attempt that failed
// after another was answered is of no interest (the server may have stopped meanwhile); if none
// was answered, the first failure is the round's.
async function settled(attempts) {
  const results = await Promise.allSettled(attempts);
  const answers = results.flatMap((r) =>
    r.status === 'fulfilled' ? [r.value] : []
  );
  const failure = results.find((r) => r.status === 'rejected');
  if (failure !== undefined && answers.every((a) => a === undefined)) {
    throw failure.reason;
  }
  return answers;
}

// How many rounds `steer` sends before it gives up.
const ROUNDS = 8;

// Sends a request until the isolate of `front` whose identity is `thread` serves it.
// `attempt(headers)` sends the request once with `headers` and resolves with what it got, or
// with undefined if another isolate refused it.
//
// The front wakes the sleeping thread with the fewest requests in flight for each request, and
// among equals the one that slept last, so one request at a time would keep going to the same warm
// thread. Steering fans out instead: a round sends several attempts a few milliseconds apart, and
// an isolate that is not the one named holds its refusal for a while, so that it has a request in
// flight and the next attempt goes to another thread. Each round sends twice as many, so that a
// thread with requests of its own in flight is reached too. The attempts share an `x-attempt`, so
// the isolate named acts on one of them only.
async function steer(thread, attempt) {
  const headers = { 'x-thread': thread, 'x-attempt': randomUUID() };
  for (let round = 1; round <= ROUNDS; round++) {
    const attempts = [];
    for (let i = 0; i < THREADS * 2 ** (round - 1); i++) {
      const sent = attempt(headers);
      // A failure is the round's only if no attempt was answered (`settled`).
      sent.catch(() => {});
      attempts.push(sent);
      await scheduler.wait(2);
    }
    const answers = (await settled(attempts)).filter(
      (answer) => answer !== undefined
    );
    if (answers.length > 0) return answers[0];
  }
  assert.fail(`isolate ${thread} served none of ${ROUNDS} rounds of requests`);
}

// A request on a connection of its own, served by whichever isolate takes it. The server answers
// once the request body ends (`/open` sends the head before): `response` is the promise for the
// parsed response, `head` for the `x-front` of its head, `finish()` ends the body.
function request(port, path = '/') {
  const req = http.request({
    agent: false,
    host: '127.0.0.1',
    port,
    path,
    method: 'POST',
  });
  let headArrived;
  const head = new Promise((resolve) => (headArrived = resolve));
  const response = new Promise((resolve, reject) => {
    req.on('error', reject);
    req.on('response', (res) => {
      headArrived(res.headers['x-front']);
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
  return { response, head, finish: () => req.end() };
}

// A request that the isolate `thread` holds in flight, once its head is here: `release()` ends it
// by resetting its connection (a client that only half-closes is not gone).
function hold(port, thread) {
  return steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request({
          agent: false,
          host: '127.0.0.1',
          port,
          path: '/hold?ms=20000',
          headers,
        });
        req.on('error', reject);
        req.on('response', (res) => {
          if (res.statusCode === 409) {
            res.resume();
            res.on('end', () => resolve(undefined));
            return;
          }
          res.on('error', () => {});
          req.on('error', () => {});
          resolve({ release: () => res.socket.resetAndDestroy() });
        });
        req.end();
      })
  );
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
// `front` have served them; their responses, in order. Nothing ties a connection to a thread,
// but the front gives a request to the thread that served the last if it sleeps with nothing in
// flight, so the thread that served one is then held busy with a request of another connection
// (`hold`) that stays in flight: the next request goes to a thread that has none.
async function oneConnection(port, threads = THREADS) {
  const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
  const sockets = new Set();
  const responses = [];
  const fronts = new Set();
  const holds = [];
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
      if (fronts.has(ids.front)) continue;
      fronts.add(ids.front);
      if (fronts.size < threads) holds.push(await hold(port, ids.front));
    }
  } finally {
    agent.destroy();
    for (const { release } of holds) release();
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
      // Requests in flight whose responses wait for their bodies: the front gives each to a
      // sleeping thread with the fewest in flight, so every thread gets some.
      const requests = Array.from({ length: fronts.length * 16 }, () =>
        request(port, '/open')
      );
      const heads = await Promise.all(requests.map(({ head }) => head));
      assert.deepStrictEqual(new Set(heads), new Set(fronts));
      workerd.kill('SIGTERM');
      // A draining server accepts no connection, and still serves the ones it has.
      await scheduler.wait(500);
      for (const { finish } of requests) finish();
      const responses = await Promise.all(
        requests.map(({ response }) => response)
      );
      assert.deepStrictEqual(
        responses.map((ids) => ids.front),
        heads
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
