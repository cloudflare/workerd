// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `workerd serve --threads --loops`: one scheduler's threads run several event loops, which move
// between threads, and by default every loop serves the one isolate of each Worker.
//
// The server runs with WORKERD_TEST_REPORT_LOOP set, so every response to a request of an HTTP
// socket says which loop served it, which thread dispatched it and which sent its head.
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import http from 'node:http';
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
  env.WORKERD_SHARED_CONFIG !== undefined,
  'You must set the WORKERD_SHARED_CONFIG environment variable.'
);
assert(
  env.WORKERD_TEST_REPORT_LOOP !== undefined,
  'You must set the WORKERD_TEST_REPORT_LOOP environment variable.'
);

const UUID = /^[0-9a-f-]{36}$/;
const LOOPS = ['--experimental', '--threads=4', '--loops=8'];

// How the server ends on SIGTERM, as `[code, signal]`.
const SIGTERM_EXIT =
  process.platform === 'win32' ? [null, 'SIGTERM'] : [0, null];
// `--loops` is a usage error on Windows.
const SEVERAL_LOOPS = {
  skip:
    process.platform === 'win32' &&
    'a work-stealing scheduler is not supported on Windows',
};

// Starts `workerd serve` with `extraArgs` and returns the harness and the port it listens on.
// The server inherits this process's environment, WORKERD_TEST_REPORT_LOOP with it.
async function serve(extraArgs = LOOPS) {
  const workerd = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: env.WORKERD_SHARED_CONFIG,
    listenPortNames: ['http'],
    extraArgs,
    // `--loops` does not support the inspector.
    inspector: false,
  });
  await workerd.start();
  return { workerd, port: await workerd.getListenPort('http') };
}

// Runs `body` with a server started with `extraArgs`, which then stops on SIGTERM.
async function withServer(extraArgs, body) {
  const { workerd, port } = await serve(extraArgs);
  try {
    await body(port);
  } finally {
    const [code, signal] = await workerd.stop();
    assert.deepStrictEqual([code, signal], SIGTERM_EXIT);
  }
}

// Where a response was served, from its `x-workerd-test-loop` header.
function servedBy(headers) {
  const header = headers['x-workerd-test-loop'];
  assert.match(header ?? '', /^\d+ \d+ \d+$/, `x-workerd-test-loop: ${header}`);
  const [loop, dispatch, send] = header.split(' ').map(Number);
  return { loop, dispatch, send };
}

// A request on a connection of its own. `response` is the promise for `{ status, body, at }`,
// where `at` is where it was served; `finish()` ends the request body.
function request(port, path, method = 'POST') {
  const agent = new http.Agent({ keepAlive: false });
  const req = http.request({
    agent,
    host: '127.0.0.1',
    port,
    path,
    method,
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
          resolve({ status: res.statusCode, body, at: servedBy(res.headers) });
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

// The response to a GET of `path`, on a connection of its own.
async function get(port, path) {
  const { response, finish } = request(port, path, 'GET');
  finish();
  return await response;
}

// The parsed JSON body of a response, which must be a 200.
function json({ status, body }) {
  assert.strictEqual(status, 200, body);
  return JSON.parse(body);
}

// Opens `count` POSTs of `path` at once and holds their bodies open, so that many loops have a
// request in flight together, then finishes them; their responses.
async function concurrentRequests(port, count, path = '/count') {
  const requests = Array.from({ length: count }, () => request(port, path));
  await scheduler.wait(100);
  for (const { finish } of requests) finish();
  return await Promise.all(requests.map(({ response }) => response));
}

// `count` GETs of `path` at once; their responses.
async function concurrentGets(port, count, path) {
  return await Promise.all(
    Array.from({ length: count }, (_, i) =>
      get(port, typeof path === 'function' ? path(i) : path)
    )
  );
}

// Asserts that `counts` are exactly 1 to their number.
function assertExactCounts(counts, what) {
  assert.deepStrictEqual(
    [...counts].sort((a, b) => a - b),
    Array.from({ length: counts.length }, (_, i) => i + 1),
    what
  );
}

// What the responses `at` say about where they were served: the loops and the threads seen.
class Where {
  loops = new Set();
  threads = new Set();
  dispatchers = new Set();
  moved = 0;

  add(at) {
    this.loops.add(at.loop);
    this.dispatchers.add(at.dispatch);
    this.threads.add(at.dispatch).add(at.send);
    if (at.dispatch !== at.send) this.moved++;
  }
}

// A WebSocket of its own connection, opened with a request for `path`: just enough of the
// protocol for short text messages. `at` is where the upgrade was served.
function webSocket(port, path) {
  return new Promise((resolve, reject) => {
    const req = http.request({
      agent: new http.Agent({ keepAlive: false }),
      host: '127.0.0.1',
      port,
      path,
      headers: {
        Connection: 'Upgrade',
        Upgrade: 'websocket',
        'Sec-WebSocket-Version': '13',
        'Sec-WebSocket-Key': randomBytes(16).toString('base64'),
      },
    });
    req.on('error', reject);
    req.on('response', (res) => {
      reject(new Error(`no upgrade: ${res.statusCode}`));
    });
    req.on('upgrade', (res, socket, head) => {
      const received = [];
      const waiting = [];
      let closed = false;
      let buffer = head;
      const deliver = (message) => {
        const waiter = waiting.shift();
        if (waiter === undefined) received.push(message);
        else waiter(message);
      };
      const parse = () => {
        while (buffer.length >= 2) {
          const opcode = buffer[0] & 0x0f;
          let length = buffer[1] & 0x7f;
          let offset = 2;
          assert(length < 127, 'a long frame');
          if (length === 126) {
            if (buffer.length < 4) return;
            length = buffer.readUInt16BE(2);
            offset = 4;
          }
          if (buffer.length < offset + length) return;
          const payload = buffer.subarray(offset, offset + length);
          buffer = buffer.subarray(offset + length);
          if (opcode === 1) deliver(payload.toString('utf8'));
          if (opcode === 8) closed = true;
        }
      };
      socket.on('data', (data) => {
        buffer = Buffer.concat([buffer, data]);
        parse();
      });
      socket.on('close', () => {
        closed = true;
        while (waiting.length > 0) waiting.shift()(null);
      });
      parse();
      const frame = (opcode, payload) => {
        const mask = randomBytes(4);
        const masked = Buffer.from(payload);
        for (let i = 0; i < masked.length; i++) masked[i] ^= mask[i % 4];
        assert(masked.length < 126);
        socket.write(
          Buffer.concat([
            Buffer.from([0x80 | opcode, 0x80 | masked.length]),
            mask,
            masked,
          ])
        );
      };
      let at;
      try {
        at = servedBy(res.headers);
      } catch (error) {
        socket.destroy();
        reject(error);
        return;
      }
      resolve({
        at,
        send: (text) => frame(1, Buffer.from(text)),
        // The next text message; null once the socket closed.
        next: () =>
          new Promise((resolve) => {
            if (received.length > 0) resolve(received.shift());
            else if (closed) resolve(null);
            else waiting.push(resolve);
          }),
        // Closes the WebSocket, then the connection once the server answered; whether it did.
        close: async () => {
          frame(8, Buffer.from([0x03, 0xe8]));
          for (let i = 0; i < 200 && !closed; i++) await scheduler.wait(10);
          const answered = closed;
          socket.destroy();
          return answered;
        },
      });
    });
    req.end();
  });
}

// The document `/rewrite` answers with: the one `shared-worker.mjs` streams, rewritten.
function rewrittenDocument() {
  let html = '<!doctype html><html><head><title>t</title></head><body>\n';
  for (let n = 0; n < 200; n++) {
    html += `<p data-n="${n}">rewritten ${n * 2}</p>\n`;
  }
  return html + '</body></html>\n';
}

test('one isolate serves every loop', SEVERAL_LOOPS, () =>
  withServer(LOOPS, async (port) => {
    const isolates = new Set();
    const counts = [];
    const where = new Where();
    // Which loop serves a connection and which thread runs it is the scheduler's choice, so try
    // until several did, rather than expect a distribution.
    for (let round = 0; round < 20; round++) {
      for (const response of await concurrentRequests(port, 64)) {
        const { isolate, count } = json(response);
        assert.match(isolate, UUID);
        isolates.add(isolate);
        counts.push(count);
        where.add(response.at);
      }
      if (round >= 2 && where.loops.size > 1 && where.dispatchers.size > 1) {
        break;
      }
    }
    assert.strictEqual(isolates.size, 1, `${isolates.size} isolates`);
    assertExactCounts(counts, 'the counts of the one isolate');
    assert(where.loops.size > 1, 'only one loop served requests');
    assert(where.dispatchers.size > 1, 'only one thread dispatched requests');
  })
);

test('a request resumes on another thread', SEVERAL_LOOPS, () =>
  withServer(LOOPS, async (port) => {
    const where = new Where();
    for (let batch = 0; batch < 20 && where.moved === 0; batch++) {
      for (const response of await concurrentGets(port, 32, '/resume')) {
        assert.strictEqual(response.status, 200, response.body);
        assert.strictEqual(response.body, 'resumed');
        where.add(response.at);
      }
    }
    assert(where.moved > 0, 'no request was sent from another thread');
  })
);

test('timers fire on every loop', SEVERAL_LOOPS, () =>
  withServer(LOOPS, async (port) => {
    const where = new Where();
    for (const response of await concurrentGets(port, 64, '/timers')) {
      const { wait, timeout } = json(response);
      assert(wait >= 45 && wait < 5000, `scheduler.wait(50) took ${wait} ms`);
      assert(
        timeout >= 15 && timeout < 5000,
        `setTimeout(20) took ${timeout} ms`
      );
      where.add(response.at);
    }
    assert(where.loops.size > 1, 'only one loop served requests');
  })
);

test('waitUntil outlives the response', SEVERAL_LOOPS, () =>
  withServer(LOOPS, async (port) => {
    const keys = Array.from({ length: 32 }, (_, i) => `key-${i}`);
    const responses = await concurrentGets(
      port,
      keys.length,
      (i) => `/wait-until?key=${keys[i]}`
    );
    for (const response of responses) {
      assert.strictEqual(response.status, 200, response.body);
      assert.strictEqual(response.body, 'responded');
    }
    let pending = keys;
    const deadline = Date.now() + 5000;
    while (pending.length > 0 && Date.now() < deadline) {
      const answers = await Promise.all(
        pending.map(async (key) => json(await get(port, `/done?key=${key}`)))
      );
      pending = pending.filter((_, i) => !answers[i].done);
      if (pending.length > 0) await scheduler.wait(50);
    }
    assert.deepStrictEqual(pending, [], 'tasks that never ended');
  })
);

test('a service binding reaches the same shared Worker', SEVERAL_LOOPS, () =>
  withServer(LOOPS, async (port) => {
    const fronts = new Set();
    const backs = new Set();
    const counts = [];
    const where = new Where();
    for (let round = 0; round < 3; round++) {
      for (const response of await concurrentRequests(port, 64, '/back')) {
        const { front, back } = json(response);
        fronts.add(front);
        backs.add(back.isolate);
        counts.push(back.count);
        where.add(response.at);
      }
    }
    assert.strictEqual(fronts.size, 1, `${fronts.size} isolates of front`);
    assert.strictEqual(backs.size, 1, `${backs.size} isolates of back`);
    assert.notStrictEqual([...fronts][0], [...backs][0]);
    assertExactCounts(counts, 'the counts of back');
    assert(where.loops.size > 1, 'only one loop served requests');
  })
);

test('HTMLRewriter with asynchronous handlers', SEVERAL_LOOPS, () =>
  withServer(LOOPS, async (port) => {
    const expected = rewrittenDocument();
    for (let round = 0; round < 3; round++) {
      for (const response of await concurrentGets(port, 16, '/rewrite')) {
        assert.strictEqual(response.status, 200, response.body);
        assert(
          response.body === expected,
          `a rewritten document of ${response.body.length} characters, not ` +
            `${expected.length}: ${JSON.stringify(response.body.slice(0, 200))}`
        );
      }
    }
  })
);

test('Durable Objects and WebSockets from every loop', SEVERAL_LOOPS, () =>
  withServer(LOOPS, async (port) => {
    const objects = new Set();
    const counts = [];
    const where = new Where();
    for (let round = 0; round < 3; round++) {
      for (const response of await concurrentRequests(port, 64, '/do/count')) {
        const { count, object } = json(response);
        objects.add(object);
        counts.push(count);
        where.add(response.at);
      }
    }
    assert.strictEqual(objects.size, 1, `${objects.size} isolates of objects`);
    assertExactCounts(counts, 'the counts of the object');
    // Only one loop hosts the object: the others reached it.
    assert(where.loops.size > 1, 'only one loop served requests');

    const sockets = await Promise.all(
      Array.from({ length: 16 }, () => webSocket(port, '/do/ws'))
    );
    const upgrades = new Where();
    try {
      await Promise.all(
        sockets.map(async (socket, s) => {
          upgrades.add(socket.at);
          for (let m = 0; m < 5; m++) {
            socket.send(`message ${s}.${m}`);
            assert.strictEqual(await socket.next(), `echo: message ${s}.${m}`);
          }
        })
      );
    } finally {
      const answered = await Promise.all(sockets.map((s) => s.close()));
      assert.deepStrictEqual(
        answered,
        sockets.map(() => true),
        'WebSockets whose close was not answered'
      );
    }
    assert(upgrades.loops.size > 1, 'only one loop served the upgrades');
  })
);

test(
  'SIGTERM drains the requests in flight on every loop',
  SEVERAL_LOOPS,
  async () => {
    const { workerd, port } = await serve();
    const requests = Array.from({ length: 64 }, () => request(port, '/count'));
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
    const where = new Where();
    for (const response of responses) {
      json(response);
      where.add(response.at);
    }
    assert(where.loops.size > 1, 'only one loop had requests in flight');
    assert.deepStrictEqual(await workerd.closed(), [0, null]);
    await assert.rejects(request(port, '/count').response);
    await workerd.stop();
  }
);

test('--isolates gives the loops that many copies', SEVERAL_LOOPS, () =>
  withServer([...LOOPS, '--isolates=2'], async (port) => {
    // The counts each isolate returned.
    const countsOf = new Map();
    for (let round = 0; round < 20; round++) {
      for (const response of await concurrentRequests(port, 64)) {
        const { isolate, count } = json(response);
        assert.match(isolate, UUID);
        if (!countsOf.has(isolate)) countsOf.set(isolate, []);
        countsOf.get(isolate).push(count);
      }
      assert(countsOf.size <= 2, `${countsOf.size} isolates of one Worker`);
      if (round >= 2 && countsOf.size === 2) break;
    }
    assert.strictEqual(countsOf.size, 2, `${countsOf.size} isolates`);
    for (const [isolate, counts] of countsOf) {
      assertExactCounts(counts, `the counts of ${isolate}`);
    }
  })
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
  'options --loops does not support are usage errors',
  SEVERAL_LOOPS,
  async () => {
    for (const [args, message] of [
      [['--threads=2', '--loops=2'], /--loops: .*`--experimental`/],
      [
        [
          '--experimental',
          '--threads=2',
          '--loops=2',
          '--inspector-addr=127.0.0.1:0',
        ],
        /--loops: --inspector-addr is not supported with --loops\./,
      ],
      [
        ['--experimental', '--threads=2', '--loops=2', '--watch'],
        /--loops: --watch is not supported with --loops\./,
      ],
      [
        ['--experimental', '--threads=2', '--loops=2', '--isolates=3'],
        /--isolates: There cannot be more copies of a Worker than loops\./,
      ],
      // The argument parser itself wants `--loops` with `--isolates`.
      [
        ['--experimental', '--threads=2', '--isolates=2'],
        /the following required arguments were not provided:\s+--loops <L>/,
      ],
    ]) {
      const { code, signal, stderr } = await serveToExit([
        env.WORKERD_SHARED_CONFIG,
        ...args,
      ]);
      assert.deepStrictEqual([code, signal], [1, null], stderr);
      assert.match(stderr, message);
    }
  }
);

test('one thread runs many loops', SEVERAL_LOOPS, () =>
  withServer(['--experimental', '--threads=1', '--loops=4'], async (port) => {
    const isolates = new Set();
    const counts = [];
    const where = new Where();
    for (let round = 0; round < 3; round++) {
      for (const response of await concurrentRequests(port, 32)) {
        const { isolate, count } = json(response);
        isolates.add(isolate);
        counts.push(count);
        where.add(response.at);
      }
    }
    assert.strictEqual(isolates.size, 1, `${isolates.size} isolates`);
    assertExactCounts(counts, 'the counts of the one isolate');
    assert.strictEqual(
      where.threads.size,
      1,
      `threads ${[...where.threads].join(', ')} served`
    );
  })
);

test(
  'loops are a usage error on Windows',
  { skip: process.platform !== 'win32' && 'only Windows refuses them' },
  async () => {
    const { code, signal, stderr } = await serveToExit([
      env.WORKERD_SHARED_CONFIG,
      '--experimental',
      '--threads=2',
      '--loops=2',
    ]);
    assert.deepStrictEqual([code, signal], [1, null], stderr);
    assert.match(
      stderr,
      /--loops: A work-stealing scheduler is not supported on Windows\./
    );
  }
);
