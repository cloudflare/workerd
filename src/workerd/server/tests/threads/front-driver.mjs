// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The front of `workerd serve --threads`: the connections of HTTP, HTTPS and TCP sockets are
// accepted and spoken by the front, each request (each TCP connection) is queued, and a loop
// thread takes it when it has nothing else to run. Most cases run on four threads and on one,
// where there is no front, and expect the same of both.
//
// Whichever thread takes a request serves it, so the tests learn the threads by the identities
// of their isolates and have the thread they choose serve what they test: a request names the
// isolate it is for (`x-thread`), any other isolate refuses it before doing anything with it
// (409), and the test sends it again until the one it names serves it. What the driver must read
// whatever thread wrote it goes through a Durable Object (`/ledger`), which every thread reaches.
import { spawn } from 'node:child_process';
import dgram from 'node:dgram';
import { randomBytes, randomUUID } from 'node:crypto';
import { once } from 'node:events';
import { mkdtempSync, readdirSync, readFileSync } from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { env } from 'node:process';
import tls from 'node:tls';
import { after, before, describe, test } from 'node:test';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WORKERD_FRONT_CONFIG !== undefined,
  'You must set the WORKERD_FRONT_CONFIG environment variable.'
);

const THREADS = 4;
const MiB = 1024 * 1024;
const UUID = /^[0-9a-f-]{36}$/;
// The certificate of the `https` socket, for 127.0.0.1 and localhost.
const CA = readFileSync(
  env.WORKERD_FRONT_CA ??
    path.join(
      path.dirname(env.WORKERD_FRONT_CONFIG),
      '../../../api/tests/connect-handler-server.pem'
    )
);
// The sockets of the config, each of which gets a `listen` event.
const SOCKETS = [
  'http',
  'alt',
  'tcp',
  'tcp-tls',
  'starttls',
  'https',
  'rewrite',
  'proxy',
  'udp',
];

// Node's SIGTERM is TerminateProcess on Windows: the server neither drains nor exits by itself
// there.
const SIGTERM_DRAINS = process.platform !== 'win32';
// How the server ends on SIGTERM, as `[code, signal]`.
const SIGTERM_EXIT =
  process.platform === 'win32' ? [null, 'SIGTERM'] : [0, null];
// The resident memory of a process can be read on Linux. Under a sanitizer it says little of
// what the server holds: freed memory stays in quarantine, and every byte touched has a shadow.
const RSS =
  process.platform === 'linux' &&
  env.ASAN_OPTIONS === undefined &&
  env.TSAN_OPTIONS === undefined;
// On Windows a server does not notice a client that goes away while nothing is read from or
// written to its connection.
const SEES_RESETS = process.platform !== 'win32';

// A marker no other request has: requests carry one so that the Worker can note what it saw.
function unique(what) {
  return `${what}-${randomUUID()}`;
}

// =====================================================================================
// The server

// Starts `workerd serve` on `threads` threads with `args`, `fds` as its descriptors from 4 on,
// and returns it once every socket of the config has its `listen` event. `onListen(event)` is
// called the moment each event is read. `log` is what the server has written so far, `events`
// every event it wrote to its control descriptor, `ports` the port of each socket.
async function serve({
  threads = THREADS,
  args = [],
  fds = [],
  onListen,
} = {}) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      env.WORKERD_FRONT_CONFIG,
      '--experimental',
      ...(threads > 1 ? [`--threads=${threads}`] : []),
      '--control-fd=3',
      ...args,
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe', ...fds] }
  );
  const server = { child, threads, log: '', events: [], ports: {} };
  for (const stream of [child.stdout, child.stderr]) {
    stream.setEncoding('utf8');
    stream.on('data', (data) => {
      server.log += data;
      process.stderr.write(data);
    });
  }
  const closed = new Promise((resolve) => {
    child.on('close', (code, signal) => resolve([code, signal]));
  });
  server.closed = () => closed;
  // How the server ends, as `[code, signal]`, once it was told to stop. A server still running
  // after `ms` is killed, and fails the test.
  server.exited = async (ms = 15_000) => {
    let timer;
    const late = new Promise((resolve) => {
      timer = setTimeout(resolve, ms, 'late');
    });
    const result = await Promise.race([closed, late]);
    clearTimeout(timer);
    if (result === 'late') {
      child.kill('SIGKILL');
      await closed;
      assert.fail(`the server still ran ${ms} ms after it was told to stop`);
    }
    return result;
  };
  server.kill = async () => {
    child.kill('SIGKILL');
    await closed;
  };
  server.stop = () => {
    child.kill('SIGTERM');
    return server.exited();
  };
  server.rss = () => {
    const status = readFileSync(`/proc/${child.pid}/status`, 'utf8');
    return Number(/VmRSS:\s+(\d+) kB/.exec(status)[1]) * 1024;
  };

  const control = child.stdio[3];
  control.setEncoding('utf8');
  let buffer = '';
  const listening = new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`no listen events in 30 seconds:\n${server.log}`)),
      30_000
    );
    control.on('data', (data) => {
      buffer += data;
      let newline;
      while ((newline = buffer.indexOf('\n')) !== -1) {
        const event = JSON.parse(buffer.slice(0, newline));
        buffer = buffer.slice(newline + 1);
        server.events.push(event);
        if (event.event !== 'listen') continue;
        server.ports[event.socket] = event.port;
        onListen?.(event);
        if (SOCKETS.every((name) => name in server.ports)) {
          clearTimeout(timer);
          resolve();
        }
      }
    });
    control.on('end', () => {
      clearTimeout(timer);
      reject(new Error(`the server ended its events:\n${server.log}`));
    });
  });
  try {
    await listening;
  } catch (error) {
    await server.kill();
    throw error;
  }
  server.http = { port: server.ports.http, threads };
  return server;
}

// Runs `body` with a server started with `options`, which then stops on SIGTERM. A server whose
// test failed is killed.
async function withServer(options, body) {
  const server = await serve(options);
  try {
    await body(server);
  } catch (error) {
    await server.kill();
    throw error;
  }
  assert.deepStrictEqual(await server.stop(), SIGTERM_EXIT);
}

// Starts a server shared by the tests of the `describe` it is called in, and stops it after
// them; the server, once started.
function sharedServer(options) {
  const shared = {};
  before(async () => {
    shared.server = await serve(options);
  });
  after(async () => {
    const { server } = shared;
    if (server === undefined) return;
    if (server.child.exitCode !== null || server.child.signalCode !== null) {
      return;
    }
    assert.deepStrictEqual(await server.stop(), SIGTERM_EXIT);
  });
  return shared;
}

// =====================================================================================
// HTTP

// How many requests a test sends before it gives up on what it waits for.
const TRIES = 200;

// How long an attempt of `steer` may take to settle before it fails.
const ATTEMPT_MS = 30_000;

// Whether a response with `status` and `headers` to a request that names the isolate `thread`
// is the refusal the Worker gives it on any other isolate.
function refusal(thread, status, headers) {
  return (
    thread !== undefined &&
    status === 409 &&
    headers['x-front'] !== undefined &&
    headers['x-front'] !== thread
  );
}

// What the `attempts` of a round came to, once every one has settled: `{ answers, refused }`,
// what the attempts that were answered resolved with, and how many were refused (resolved with
// undefined). If any attempt rejected, the answers are disposed of with `discard`, and it fails
// with an error that says how many of the round's attempts failed, whose cause is the first
// failure; a failure of `discard` counts as one.
async function settled(attempts, discard) {
  const results = await Promise.allSettled(attempts);
  const answers = [];
  const failures = [];
  let refused = 0;
  for (const result of results) {
    if (result.status === 'rejected') failures.push(result.reason);
    else if (result.value === undefined) refused++;
    else answers.push(result.value);
  }
  if (failures.length > 0) {
    if (discard !== undefined) {
      for (const result of await Promise.allSettled(
        answers.map(async (answer) => discard(answer))
      )) {
        if (result.status === 'rejected') failures.push(result.reason);
      }
    }
    const [first] = failures;
    throw new Error(
      `${failures.length} of ${attempts.length} attempts failed, the first with: ` +
        `${first?.message ?? first}`,
      { cause: first }
    );
  }
  return { answers, refused };
}

// How many rounds `steer` sends before it gives up.
const ROUNDS = 6;

// Sends a request until the isolate whose identity is `thread` serves it, or once to any isolate
// if `thread` is undefined, on a server of `threads` threads. `attempt(headers)` sends the
// request once with `headers` and settles in one of three ways: it resolves with what it got
// from the isolate named, it resolves with undefined if and only if another isolate refused it
// (the 409 of `refusal`), or it rejects. An attempt that does not settle within `ATTEMPT_MS`
// rejects.
//
// The front wakes the sleeping thread with the fewest requests in flight for each request, and
// among equals the one that slept last, so one request at a time would keep going to the same warm
// thread. Steering fans out instead: a round sends several attempts a few milliseconds apart, and
// an isolate that is not the one named holds its refusal for a while, so that it has a request in
// flight and the next attempt goes to another thread. Each round sends twice as many as the last,
// up to `ROUNDS` rounds, so that a thread with requests of its own in flight is reached too. Every
// attempt of a round settles before the round ends, and any attempt that rejects fails the
// request (`settled`), whether or not another was answered. Only the isolate named acts on the
// request, but it may get more than one attempt of a round: `discard(answer)` disposes of the
// answers past the first, and a failure of it fails the request too. A request that no round
// had answered fails with the number of attempts refused in each round.
async function steer(thread, attempt, { threads = THREADS, discard } = {}) {
  const attemptOnce = (headers) =>
    within(attempt(headers), ATTEMPT_MS, 'an attempt of a request');
  if (thread === undefined) {
    const answer = await attemptOnce({});
    assert.notStrictEqual(answer, undefined, 'a request was refused');
    return answer;
  }
  const headers = { 'x-thread': thread };
  const rounds = [];
  for (let round = 1; round <= ROUNDS; round++) {
    const attempts = [];
    for (let i = 0; i < threads * 2 ** (round - 1); i++) {
      const sent = attemptOnce(headers);
      // `settled` inspects every attempt once the round has been sent; this only keeps a
      // rejection meanwhile from being reported as unhandled.
      sent.catch(() => {});
      attempts.push(sent);
      await scheduler.wait(2);
    }
    const { answers, refused } = await settled(attempts, discard);
    for (const extra of answers.slice(1)) await discard?.(extra);
    if (answers.length > 0) return answers[0];
    rounds.push(`${refused} of ${attempts.length}`);
  }
  assert.fail(
    `isolate ${thread} served none of ${ROUNDS} rounds of requests; refused in each ` +
      `round: ${rounds.join(', ')}`
  );
}

// The options of a request for `path` to `target`: `{ port }`, `{ port, tls: true }` or
// `{ socketPath }`.
function options(target, path, more = {}) {
  const where =
    target.socketPath !== undefined
      ? { socketPath: target.socketPath }
      : { host: '127.0.0.1', port: target.port };
  const secure = target.tls ? { ca: CA, servername: 'localhost' } : {};
  return { agent: false, ...where, ...secure, path, ...more };
}

// One request for `path` to `target` (see `options`), its body `body` if given: resolves with
// `{ status, headers, body, socket }` once the response ended.
function send(target, path, { body, ...more } = {}) {
  return new Promise((resolve, reject) => {
    const req = (target.tls ? https : http).request(
      options(target, path, more),
      (res) => {
        // A keep-alive agent takes the socket back once the response ended.
        const { socket } = res;
        const chunks = [];
        res.on('data', (chunk) => chunks.push(chunk));
        res.on('error', reject);
        res.on('end', () =>
          resolve({
            status: res.statusCode,
            headers: res.headers,
            body: Buffer.concat(chunks),
            socket,
          })
        );
      }
    );
    req.setTimeout(20_000, () => {
      req.destroy(new Error(`no response to ${path} in 20 seconds`));
    });
    req.on('error', reject);
    req.end(body);
  });
}

// The parsed JSON response to a GET of `path` on `target`, served by the isolate `thread` (any
// if undefined). `more` are further options of the request.
function get(target, path, thread, more = {}) {
  return steer(
    thread,
    async (headers) => {
      const res = await send(target, path, {
        ...more,
        headers: { ...more.headers, ...headers },
      });
      const text = res.body.toString();
      if (refusal(thread, res.status, res.headers)) return undefined;
      assert.strictEqual(res.status, 200, text);
      const answer = JSON.parse(text);
      if (thread !== undefined) assert.strictEqual(answer.front, thread);
      return answer;
    },
    { threads: target.threads }
  );
}

// What the ledger, which every thread reaches, holds under `key`, once it holds anything.
async function ledgerValue(server, key) {
  for (let tries = 0; tries < TRIES * 5; tries++) {
    const { value } = await get(server.http, `/ledger?key=${key}`);
    if (value !== null) return value;
    await scheduler.wait(10);
  }
  assert.fail(`the ledger has nothing under ${key}`);
}

// What the isolate `thread` noted of the request with `marker`, once `done(notes)` holds; fails
// if it does not within `ms`.
async function notesOf(server, thread, marker, done, ms = 10_000) {
  const until = Date.now() + ms;
  for (;;) {
    const { notes } = await get(server.http, `/notes?of=${marker}`, thread);
    if (done(notes)) return notes;
    assert(
      Date.now() < until,
      `${JSON.stringify(notes)} is all the Worker noted of ${marker}`
    );
    await scheduler.wait(20);
  }
}

// The isolate of `front` on the server's first thread, which handles SIGTERM: the one thread
// that serves the datagram socket, whose Worker asks the `front` of its thread.
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

// The isolates of `front`, one per thread, by identity: `first` is the one the `Ledger` object
// lives on, its home thread; `replicas` are the others'. A round sends a request per thread at
// once, each answered after a while: a thread with one in flight is passed over for one with
// none, so a round usually finds every thread.
async function threadsOf(server) {
  const fronts = new Set();
  let first;
  for (let round = 0; round < ROUNDS && fronts.size < server.threads; round++) {
    const answers = [];
    for (let i = 0; i < server.threads; i++) {
      answers.push(get(server.http, '/where?ms=250'));
      await scheduler.wait(2);
    }
    for (const where of await Promise.all(answers)) {
      assert.match(where.front, UUID);
      assert.strictEqual(first ?? where.first, where.first);
      first = where.first;
      fronts.add(where.front);
    }
  }
  assert.strictEqual(fronts.size, server.threads, 'isolates that served');
  assert(fronts.has(first), 'the first thread served no request');
  const replicas = [...fronts].filter((front) => front !== first);
  return { first, replicas, all: [first, ...replicas] };
}

// The isolate the tests send to: one of a thread without the objects, if there is one.
function away({ first, replicas }) {
  return replicas[0] ?? first;
}

// A request to `target`, a TCP socket without TLS, that the isolate `thread` holds in flight
// for `ms` milliseconds (any isolate if undefined), once its head is here: `{ front, release }`,
// where `release()` ends it, by hanging up where the server notices that, and elsewhere by
// asking the isolate to end it.
function hold(target, thread, ms = 20_000) {
  return steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const id = unique('hold');
        const req = http.request(
          options(target, `/hold?ms=${ms}&id=${id}`, { headers })
        );
        req.on('error', reject);
        req.on('response', (res) => {
          res.on('error', reject);
          if (refusal(thread, res.statusCode, res.headers)) {
            res.resume();
            res.on('end', () => resolve(undefined));
            return;
          }
          if (res.statusCode !== 200) {
            res.resume();
            reject(new Error(`a hold was answered ${res.statusCode}`));
            return;
          }
          res.on('error', () => {});
          req.on('error', () => {});
          const front = res.headers['x-front'];
          // A client that only half-closes is not gone while its request is in flight: the
          // release resets the connection. On Windows the server does not notice that while
          // it neither reads from nor writes to the connection, so the isolate is told.
          const release = async () => {
            if (!SEES_RESETS) {
              await get(target, `/release?id=${id}`, front);
            }
            res.socket.resetAndDestroy();
          };
          resolve({ front, release });
        });
        req.end();
      }),
    { threads: target.threads, discard: (extra) => extra.release() }
  );
}

// Sends GETs of `/` to `target` one after another over one keep-alive connection until every
// thread of `server` has served one; the isolates, in the order they served. The front would
// give each to the thread that served the last, which is warm, so the thread that served one is
// then held busy with a request of another connection to `server.http` (`hold`) that stays in
// flight: the next request goes to a thread that has none.
async function oneConnection(server, target = server.http) {
  const agent = new (target.tls ? https : http).Agent({
    keepAlive: true,
    maxSockets: 1,
  });
  const sockets = new Set();
  const fronts = [];
  const holds = [];
  try {
    for (
      let tries = 0;
      tries < TRIES && fronts.length < server.threads;
      tries++
    ) {
      const res = await send(target, '/', { agent });
      assert.strictEqual(res.status, 200);
      sockets.add(res.socket);
      const { front } = JSON.parse(res.body);
      if (fronts.includes(front)) continue;
      fronts.push(front);
      if (fronts.length < server.threads) {
        holds.push(await hold(server.http, front));
      }
    }
  } finally {
    agent.destroy();
    await Promise.all(holds.map(({ release }) => release()));
  }
  assert.strictEqual(sockets.size, 1, 'connections the requests went over');
  assert.strictEqual(
    fronts.length,
    server.threads,
    'isolates that served one connection'
  );
  return fronts;
}

// Has a thread run JavaScript for `ms` milliseconds, or until the time `ms.until` (epoch
// milliseconds) if `ms` is `{ until }`: the isolate `thread` (any if undefined) answers, and
// once its response head is here the thread spins in the body's first pull. Resolves then with
// `{ front, headAt, done }`; `done` resolves with the Worker's `start` and `end` of the spin and
// `endAt`, when the body ended here. An isolate that got a second attempt spins for it too; its
// connection is closed.
function spin(server, ms, thread) {
  const query = typeof ms === 'object' ? `until=${ms.until}` : `ms=${ms}`;
  return steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(
          options(server.http, `/spin?${query}`, { headers })
        );
        req.on('error', reject);
        req.on('response', (res) => {
          if (refusal(thread, res.statusCode, res.headers)) {
            res.on('error', reject);
            res.resume();
            res.on('end', () => resolve(undefined));
            return;
          }
          if (res.statusCode !== 200) {
            res.resume();
            reject(new Error(`a spin was answered ${res.statusCode}`));
            return;
          }
          const headAt = Date.now();
          let body = '';
          res.setEncoding('utf8');
          const done = new Promise((resolve, reject) => {
            res.on('data', (chunk) => (body += chunk));
            res.on('error', reject);
            res.on('end', () => {
              try {
                resolve({ ...JSON.parse(body), endAt: Date.now() });
              } catch (error) {
                reject(error);
              }
            });
          });
          done.catch(() => {});
          resolve({ front: res.headers['x-front'], headAt, done, req });
        });
        req.end();
      }),
    { threads: server.threads, discard: ({ req }) => req.destroy() }
  );
}

// A connection to the server's `port` (`{ tls: true }` for TLS), once it is open.
async function connect(port, { tls: secure = false } = {}) {
  const socket = secure
    ? tls.connect({ host: '127.0.0.1', port, ca: CA, servername: 'localhost' })
    : net.connect({ host: '127.0.0.1', port });
  await once(socket, secure ? 'secureConnect' : 'connect');
  return socket;
}

// What arrives on a socket, for reading at the pace the test chooses.
class Incoming {
  #buffer = Buffer.alloc(0);
  #waiter = null;
  ended = false;
  error = null;

  constructor(socket) {
    socket.on('data', (data) => {
      this.#buffer = Buffer.concat([this.#buffer, data]);
      this.#wake();
    });
    socket.on('end', () => {
      this.ended = true;
      this.#wake();
    });
    socket.on('close', () => {
      this.ended = true;
      this.#wake();
    });
    socket.on('error', (error) => {
      this.error = error;
      this.#wake();
    });
  }

  #wake() {
    const waiter = this.#waiter;
    this.#waiter = null;
    waiter?.();
  }

  // Waits until more has arrived or nothing more will; whether more did.
  async #more() {
    if (this.ended) return false;
    const length = this.#buffer.length;
    await new Promise((resolve) => (this.#waiter = resolve));
    return this.#buffer.length > length;
  }

  // The next `n` bytes; fails if the socket ends first.
  async bytes(n) {
    while (this.#buffer.length < n) {
      if (!(await this.#more()) && this.ended && this.#buffer.length < n) {
        throw new Error(
          `the connection ended with ${this.#buffer.length} of ${n} bytes: ${this.error}`
        );
      }
    }
    const bytes = this.#buffer.subarray(0, n);
    this.#buffer = this.#buffer.subarray(n);
    return bytes;
  }

  // Everything up to and including `delimiter`, as text.
  async until(delimiter) {
    for (;;) {
      const at = this.#buffer.indexOf(delimiter);
      if (at !== -1) {
        return (await this.bytes(at + delimiter.length)).toString('latin1');
      }
      if (!(await this.#more()) && this.ended) {
        throw new Error(
          `the connection ended before ${JSON.stringify(delimiter)}: ` +
            JSON.stringify(this.#buffer.toString('latin1'))
        );
      }
    }
  }

  // Waits for the end of the connection; what arrived before it.
  async rest() {
    while (!this.ended) await this.#more();
    const rest = this.#buffer;
    this.#buffer = Buffer.alloc(0);
    return rest;
  }

  // The head of the next response: `{ status, headers }`, header names in lower case.
  async head() {
    const lines = (await this.until('\r\n\r\n')).split('\r\n');
    const status = Number(/^HTTP\/1\.1 (\d{3}) /.exec(lines[0])[1]);
    const headers = {};
    for (const line of lines.slice(1, -2)) {
      const colon = line.indexOf(':');
      headers[line.slice(0, colon).toLowerCase()] = line
        .slice(colon + 1)
        .trim();
    }
    return { status, headers };
  }

  // The next response: `{ status, headers, body }`.
  async response() {
    const head = await this.head();
    let body;
    if (head.headers['transfer-encoding'] === 'chunked') {
      const chunks = [];
      for (;;) {
        const size = parseInt(await this.until('\r\n'), 16);
        chunks.push(await this.bytes(size));
        await this.bytes(2);
        if (size === 0) break;
      }
      body = Buffer.concat(chunks);
    } else if (head.headers['content-length'] !== undefined) {
      body = await this.bytes(Number(head.headers['content-length']));
    } else {
      body = await this.rest();
    }
    return { ...head, body };
  }
}

// Writes `data` to `socket` and waits until it was handed to the system.
function write(socket, data) {
  return new Promise((resolve, reject) => {
    socket.write(data, (error) => (error ? reject(error) : resolve()));
  });
}

// =====================================================================================
// WebSockets

// One WebSocket client end, over `socket`, whose upgrade has been answered.
class WebSocketClient {
  #socket;
  #incoming;

  constructor(socket, incoming, headers) {
    this.#socket = socket;
    this.#incoming = incoming;
    this.headers = headers;
    this.closed = new Promise((resolve) => socket.on('close', resolve));
  }

  #frame(opcode, payload) {
    const mask = randomBytes(4);
    const masked = Buffer.from(payload);
    for (let i = 0; i < masked.length; i++) masked[i] ^= mask[i % 4];
    let head;
    if (masked.length < 126) {
      head = Buffer.from([0x80 | opcode, 0x80 | masked.length]);
    } else if (masked.length < 65536) {
      head = Buffer.alloc(4);
      head.writeUInt16BE(masked.length, 2);
      head[1] = 0x80 | 126;
    } else {
      head = Buffer.alloc(10);
      head.writeBigUInt64BE(BigInt(masked.length), 2);
      head[1] = 0x80 | 127;
    }
    head[0] = 0x80 | opcode;
    this.#socket.write(Buffer.concat([head, mask, masked]));
  }

  sendText(text) {
    this.#frame(1, Buffer.from(text));
  }

  sendBinary(data) {
    this.#frame(2, data);
  }

  close(code, reason) {
    const payload = Buffer.alloc(2 + Buffer.byteLength(reason));
    payload.writeUInt16BE(code);
    payload.write(reason, 2);
    this.#frame(8, payload);
  }

  // The next message: `{ text }`, `{ binary }` or `{ code, reason }` for a close.
  async next() {
    const parts = [];
    for (;;) {
      const [first, second] = await this.#incoming.bytes(2);
      assert.strictEqual(second & 0x80, 0, 'a masked frame from the server');
      let length = second & 0x7f;
      if (length === 126)
        length = (await this.#incoming.bytes(2)).readUInt16BE();
      else if (length === 127) {
        length = Number((await this.#incoming.bytes(8)).readBigUInt64BE());
      }
      const payload = Buffer.from(await this.#incoming.bytes(length));
      const opcode = first & 0x0f;
      if (opcode === 9) {
        this.#frame(10, payload);
        continue;
      }
      if (opcode === 10) continue;
      if (opcode === 8) {
        return {
          code: payload.length >= 2 ? payload.readUInt16BE() : null,
          reason: payload.subarray(2).toString(),
        };
      }
      if (opcode !== 0) parts.type = opcode;
      parts.push(payload);
      if (first & 0x80) {
        const data = Buffer.concat(parts);
        return parts.type === 1 ? { text: data.toString() } : { binary: data };
      }
    }
  }

  destroy() {
    this.#socket.destroy();
  }
}

// A WebSocket opened with a request for `path` over a connection `open()` makes, served by the
// isolate `thread` (any if undefined) of a server on `threads` threads.
function webSocket(open, path, thread, threads) {
  return steer(
    thread,
    async (headers) => {
      const socket = await open();
      try {
        const incoming = new Incoming(socket);
        const extra = Object.entries(headers)
          .map(([name, value]) => `${name}: ${value}\r\n`)
          .join('');
        socket.write(
          `GET ${path} HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\n` +
            'Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n' +
            `Sec-WebSocket-Key: ${randomBytes(16).toString('base64')}\r\n${extra}\r\n`
        );
        const head = await incoming.head();
        if (refusal(thread, head.status, head.headers)) {
          socket.destroy();
          return undefined;
        }
        assert.strictEqual(head.status, 101);
        return new WebSocketClient(socket, incoming, head.headers);
      } catch (error) {
        socket.destroy();
        throw error;
      }
    },
    { threads, discard: (extra) => extra.destroy() }
  );
}

// =====================================================================================
// The pattern of the bodies the tests check: byte `i` of a body is `i % 251`.

const PATTERN = Buffer.from(
  Uint8Array.from({ length: 251 + 64 * 1024 }, (_, i) => i % 251)
);

// Whether `chunk`, found at `offset` of a body, is the pattern.
function isPattern(chunk, offset) {
  for (let at = 0; at < chunk.length; at += 64 * 1024) {
    const piece = chunk.subarray(at, at + 64 * 1024);
    const start = (offset + at) % 251;
    if (!piece.equals(PATTERN.subarray(start, start + piece.length))) {
      return false;
    }
  }
  return true;
}

// The pattern from `offset` on, `length` bytes of it.
function pattern(offset, length) {
  const chunk = Buffer.alloc(length);
  for (let at = 0; at < length; at += 64 * 1024) {
    const start = (offset + at) % 251;
    PATTERN.copy(chunk, at, start, start + Math.min(64 * 1024, length - at));
  }
  return chunk;
}

// =====================================================================================
// Helpers of the cases

// Resolves with what `promise` does, or fails if it does not settle within `ms`.
async function within(promise, ms, what) {
  let timer;
  const late = new Promise((_, reject) => {
    timer = setTimeout(
      () => reject(new Error(`${what} took more than ${ms} ms`)),
      ms
    );
  });
  try {
    return await Promise.race([promise, late]);
  } finally {
    clearTimeout(timer);
  }
}

// How many descriptors the server has open (Linux).
function descriptors(server) {
  return readdirSync(`/proc/${server.child.pid}/fd`).length;
}

// Waits until `condition()` holds, checking every few milliseconds; fails after `ms`.
async function until(condition, ms, what) {
  const end = Date.now() + ms;
  while (!condition()) {
    assert(Date.now() < end, `${what} did not happen within ${ms} ms`);
    await scheduler.wait(10);
  }
}

// Resolves once `req` may be written to again, and fails if it fails first.
function drained(req) {
  return new Promise((resolve, reject) => {
    const done = (error) => {
      req.off('drain', done);
      req.off('error', done);
      req.off('close', closed);
      if (error) reject(error);
      else resolve();
    };
    const closed = () => done(new Error('the request closed while writing'));
    req.on('drain', done);
    req.on('error', done);
    req.on('close', closed);
  });
}

// A GET of `/` on a connection of its own: who served it, how long it took, and when it ended.
async function timedGet(server) {
  const sent = Date.now();
  const { front } = await get(server.http, '/');
  const at = Date.now();
  return { front, took: at - sent, at };
}

// Uploads `size` bytes to `/upload`, which reads the first `slow` of them at `rate` bytes a
// second. While the Worker reads slowly the client samples how far its writes are ahead of what
// the Worker read (`ahead`), and throughout the server's resident memory (`rssGrowth`, past what
// it was before).
async function upload(server, { size, slow = 0, rate = 1 }) {
  const marker = unique('upload');
  const rssBefore = RSS ? server.rss() : 0;
  let rssMax = rssBefore;
  const req = http.request(
    options(server.http, `/upload?marker=${marker}&slow=${slow}&rate=${rate}`, {
      method: 'POST',
    })
  );
  const response = new Promise((resolve, reject) => {
    req.on('error', reject);
    req.on('response', (res) => {
      let body = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => (body += chunk));
      res.on('error', reject);
      res.on('end', () => resolve({ status: res.statusCode, body }));
    });
  });
  response.catch(() => {});
  const CHUNK = 256 * 1024;
  const chunk = Buffer.alloc(CHUNK, 120);
  let written = 0;
  let writing = true;
  const writer = (async () => {
    try {
      while (written < size) {
        const more = req.write(chunk);
        written += CHUNK;
        if (!more) await drained(req);
      }
      req.end();
    } finally {
      writing = false;
    }
  })();
  const ahead = [];
  const sampler = (async () => {
    while (writing) {
      if (RSS) rssMax = Math.max(rssMax, server.rss());
      if (slow > 0) {
        const { value } = await get(server.http, `/ledger?key=${marker}`);
        const consumed = value ?? 0;
        if (consumed < slow) ahead.push(written - consumed);
      }
      await scheduler.wait(50);
    }
  })();
  await writer;
  await sampler;
  const res = await within(response, 60_000, 'the upload response');
  assert.strictEqual(res.status, 200, res.body);
  return { ...JSON.parse(res.body), ahead, rssGrowth: rssMax - rssBefore };
}

// Uploads `size` bytes to `/upload` on the isolate `thread`, which runs JavaScript for `work`
// milliseconds per MiB it reads and waits for nothing but the body. Resolves once the response
// head is here, with `{ front, done }`; `done` resolves with what the Worker read, once it
// answered.
async function busyUpload(server, thread, { size, work }) {
  const sent = await steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(
          options(server.http, `/upload?work=${work}`, {
            method: 'POST',
            headers,
          })
        );
        req.on('error', reject);
        req.on('response', (res) => {
          if (refusal(thread, res.statusCode, res.headers)) {
            // The isolate that refuses reads no body.
            res.on('error', reject);
            res.resume();
            res.on('end', () => {
              resolve(undefined);
              req.destroy();
            });
            return;
          }
          if (res.statusCode !== 200) {
            req.destroy();
            reject(new Error(`an upload was answered ${res.statusCode}`));
            return;
          }
          resolve({ req, res });
        });
        req.flushHeaders();
      }),
    { threads: server.threads, discard: ({ req }) => req.destroy() }
  );
  const { req, res } = sent;
  let body = '';
  res.setEncoding('utf8');
  res.on('data', (chunk) => (body += chunk));
  const done = new Promise((resolve, reject) => {
    req.on('error', reject);
    res.on('error', reject);
    res.on('end', () => {
      try {
        resolve(JSON.parse(body));
      } catch (error) {
        reject(error);
      }
    });
  });
  done.catch(() => {});
  const writer = (async () => {
    const chunk = Buffer.alloc(256 * 1024, 120);
    for (let written = 0; written < size; written += chunk.length) {
      if (!req.write(chunk)) await drained(req);
    }
    req.end();
  })();
  writer.catch(() => {});
  return { front: res.headers['x-front'], done };
}

// Downloads `size` bytes of the pattern from `/download`, the first `steps` times 4 MiB of them
// slowly: a step reads 4 MiB, then reads nothing for a while, and samples how far the Worker's
// production is ahead of what the client read (`ahead`) and the server's resident memory
// (`rssGrowth`, past what it was before). Checks every byte.
async function download(server, size, steps = 0) {
  const marker = unique('download');
  const rssBefore = RSS ? server.rss() : 0;
  let rssMax = rssBefore;
  const req = http.request(
    options(server.http, `/download?size=${size}&marker=${marker}`)
  );
  const res = await new Promise((resolve, reject) => {
    req.on('error', reject);
    req.on('response', resolve);
    req.end();
  });
  assert.strictEqual(res.statusCode, 200);
  res.pause();
  let received = 0;
  let intact = true;
  let target = 0;
  let reached;
  res.on('data', (chunk) => {
    if (!isPattern(chunk, received)) intact = false;
    received += chunk.length;
    if (received >= target) {
      res.pause();
      reached?.();
    }
  });
  const ended = new Promise((resolve, reject) => {
    res.on('end', resolve);
    res.on('error', reject);
  });
  ended.catch(() => {});
  const ahead = [];
  for (let step = 1; step <= steps; step++) {
    target = step * 4 * MiB;
    const reach = new Promise((resolve) => (reached = resolve));
    res.resume();
    await within(reach, 20_000, `reading ${target} bytes`);
    // A slow client: while it reads nothing, a producer that ignored it would run ahead.
    await scheduler.wait(100);
    const { value } = await get(server.http, `/ledger?key=${marker}`);
    ahead.push((value ?? 0) - received);
    if (RSS) rssMax = Math.max(rssMax, server.rss());
  }
  target = Infinity;
  res.resume();
  const sampling = setInterval(() => {
    if (RSS) rssMax = Math.max(rssMax, server.rss());
  }, 50);
  try {
    await within(ended, 60_000, 'the download');
  } finally {
    clearInterval(sampling);
  }
  assert.strictEqual(received, size, 'bytes received');
  assert(intact, 'the body is not the pattern');
  return { ahead, rssGrowth: rssMax - rssBefore };
}

// A response the server cannot finish: `{ status, headers, received, complete, error }`, once
// the connection closed; `{ status: null, bytes }` if it was reset before the head arrived,
// `bytes` what arrived before the reset.
function partial(target, path) {
  return new Promise((resolve, reject) => {
    const req = http.request(options(target, path));
    req.on('error', (error) => {
      if (error.code === 'ECONNRESET') {
        resolve({ status: null, bytes: req.socket?.bytesRead ?? 0 });
      } else {
        reject(error);
      }
    });
    req.on('response', (res) => {
      let received = 0;
      let error = null;
      res.on('data', (chunk) => (received += chunk.length));
      res.on('error', (e) => (error = e));
      res.on('close', () =>
        resolve({
          status: res.statusCode,
          headers: res.headers,
          received,
          complete: res.complete,
          error,
        })
      );
    });
    req.end();
  });
}

// A TCP session with the `connect()` handler, over a connection `open()` makes
// (`{ socket, incoming }`): data is sent before anything arrived, then in lockstep. Resolves
// with the identity of the isolate that serves it and `finish()`, which half-closes the
// connection and checks that the handler still answers, says how much it got and closes.
async function tcpSession(open) {
  const { socket, incoming } = await open();
  try {
    const sent = ['early\n', 'ping 1\n', 'ping 2\n', 'ping 3\n'];
    socket.write(sent[0]);
    const hello = /^hello ([0-9a-f-]{36})\n$/.exec(await incoming.until('\n'));
    assert(hello !== null, 'the handler greets with its identity');
    assert.strictEqual(await incoming.until('\n'), sent[0]);
    for (const line of sent.slice(1)) {
      socket.write(line);
      assert.strictEqual(await incoming.until('\n'), line);
    }
    const finish = async () => {
      try {
        socket.end();
        const bytes = sent.join('').length;
        assert.strictEqual(await incoming.until('\n'), `bye ${bytes}\n`);
        assert.strictEqual((await incoming.rest()).length, 0);
      } finally {
        socket.destroy();
      }
    };
    return { front: hello[1], finish };
  } catch (error) {
    socket.destroy();
    throw error;
  }
}

// Echoes text and binary messages, small and large, through `ws`.
async function echoMessages(ws) {
  ws.sendText('hello');
  assert.deepStrictEqual(await ws.next(), { text: 'hello' });
  for (const size of [1000, 200 * 1024]) {
    const data = randomBytes(size);
    ws.sendBinary(data);
    const { binary } = await ws.next();
    assert(binary.equals(data), `a binary message of ${size} bytes`);
  }
  const text = 'abcdefghij'.repeat(10 * 1024);
  ws.sendText(text);
  assert.deepStrictEqual(await ws.next(), { text });
}

// What the cases on one thread observed, by what: the server's options made of requests on four
// sockets, the bytes ahead of a slow upload. The cases on one thread run first, and those on
// four are compared with them.
const observed = new Map();

function observe(threads, what, value) {
  // Ports differ from run to run.
  value = JSON.parse(
    JSON.stringify(value).replaceAll(/127\.0\.0\.1:\d+/g, '127.0.0.1:<port>')
  );
  if (threads === 1) {
    observed.set(what, value);
  } else if (observed.has(what)) {
    assert.deepStrictEqual(
      value,
      observed.get(what),
      `${what} differs from one thread`
    );
  }
}

// =====================================================================================
// The cases on one server each for one thread and for four

for (const threads of [1, THREADS]) {
  const on = `on ${threads} thread${threads > 1 ? 's' : ''}`;
  const several = threads > 1;
  describe(on, () => {
    const shared = sharedServer({ threads });

    // -------------------------------------------------------------------------------
    // Dispatch

    test(`requests on one keep-alive connection are served by different threads ${on}`, async () => {
      const fronts = await oneConnection(shared.server);
      assert.strictEqual(fronts.length, threads);
    });

    test(
      `a busy thread does not delay requests other threads could take ${on}`,
      { skip: !several && 'one thread has no other' },
      async () => {
        const { server } = shared;
        // Requests sent while `spinners` spin are served by the other threads, each promptly,
        // all before the first spinner ends.
        const meanwhile = async (spinners) => {
          const busy = new Set(spinners.map((s) => s.front));
          const answers = [];
          for (let i = 0; i < 8; i++) answers.push(await timedGet(server));
          answers.push(
            ...(await Promise.all(
              Array.from({ length: 16 }, () => timedGet(server))
            ))
          );
          const spun = await Promise.all(spinners.map((s) => s.done));
          for (const [i, { start, end, endAt }] of spun.entries()) {
            // The head came while the thread had the spin ahead of it.
            assert(
              endAt - spinners[i].headAt >= end - start - 500,
              'a spinner answered late'
            );
          }
          const firstEnd = Math.min(...spun.map((s) => s.endAt));
          for (const { front, took, at } of answers) {
            assert(!busy.has(front), 'a busy thread served a request');
            assert(took < 1000, `a request took ${took} ms`);
            assert(at < firstEnd, 'a request was answered after a spin');
          }
          return new Set(answers.map((a) => a.front));
        };

        const one = await spin(server, 2000);
        await meanwhile([one]);

        // With three threads busy, the fourth serves everything.
        const three = [];
        for (let i = 0; i < threads - 1; i++) {
          three.push(await spin(server, 2500));
        }
        assert.strictEqual(new Set(three.map((s) => s.front)).size, 3);
        assert.strictEqual((await meanwhile(three)).size, 1);
      }
    );

    test(
      `with every thread busy, queued requests are served in arrival order by the first thread to free up ${on}`,
      { skip: !several && 'one thread accepts only when it is free' },
      async () => {
        const { server } = shared;
        // Every thread spins until a time chosen here, so that how long steering takes does
        // not matter: the first spinner until `T`, the others from 1500 ms later on.
        const T = Date.now() + 4000;
        const spinners = [];
        for (const after of [0, 1500, 1700, 1900]) {
          spinners.push(await spin(server, { until: T + after }));
        }
        assert.strictEqual(new Set(spinners.map((s) => s.front)).size, 4);
        const markers = Array.from({ length: 8 }, (_, n) =>
          unique(`queued-${n}`)
        );
        const responses = [];
        let lastSent;
        for (const marker of markers) {
          const socket = await connect(server.http.port);
          const incoming = new Incoming(socket);
          await write(
            socket,
            `GET /?marker=${marker} HTTP/1.1\r\nHost: localhost\r\n\r\n`
          );
          lastSent = Date.now();
          responses.push(
            incoming.response().then((res) => {
              socket.destroy();
              return res;
            })
          );
          // The front reads the request and queues it long before the next is sent, so the
          // requests are queued in the order they are sent.
          await scheduler.wait(50);
        }
        assert(
          lastSent < T - 500,
          `the last request was sent only ${T - lastSent} ms before the first spin ` +
            'ended: the test is too slow to show anything'
        );
        const answers = await within(
          Promise.all(responses),
          20_000,
          'the queued requests'
        );
        const spun = await Promise.all(spinners.map((s) => s.done));
        const firstFree = spinners[0].front;
        for (const res of answers) {
          assert.strictEqual(res.status, 200);
          const { front, start } = JSON.parse(res.body);
          assert.strictEqual(front, firstFree);
          // It was queued until the first spinner was done.
          assert(
            start >= spun[0].end,
            `a request started ${spun[0].end - start} ms before the first spin ended`
          );
        }
        const { seen } = await get(server.http, '/seen', firstFree);
        assert.deepStrictEqual(
          seen.filter((m) => markers.includes(m)),
          markers
        );
      }
    );

    test(`pipelined requests on one connection are answered in order ${on}`, async () => {
      const socket = await connect(shared.server.http.port);
      try {
        const incoming = new Incoming(socket);
        const markers = [0, 1, 2].map((n) => unique(`pipelined-${n}`));
        await write(
          socket,
          markers
            .map(
              (marker, n) =>
                `GET /sleep?ms=${n === 0 ? 500 : 0}&marker=${marker} HTTP/1.1\r\n` +
                'Host: localhost\r\n\r\n'
            )
            .join('')
        );
        for (const marker of markers) {
          const res = await incoming.response();
          assert.strictEqual(res.status, 200);
          assert.strictEqual(JSON.parse(res.body).marker, marker);
        }
      } finally {
        socket.destroy();
      }
    });

    // -------------------------------------------------------------------------------
    // Bodies

    test(`a large upload streams through with backpressure and bounded memory ${on}`, async () => {
      const { server } = shared;
      if (several) {
        // A quarter of a gigabyte at full speed, which costs too long to run twice.
        const size = 256 * MiB;
        const fast = await upload(server, { size });
        assert.strictEqual(fast.consumed, size);
        if (RSS)
          assert(fast.rssGrowth < 150 * MiB, `RSS grew ${fast.rssGrowth}`);
      }

      // The Worker reads the first 32 MiB at 8 MiB a second.
      const slow = await upload(server, {
        size: 48 * MiB,
        slow: 32 * MiB,
        rate: 8 * MiB,
      });
      assert.strictEqual(slow.consumed, 48 * MiB);
      assert(slow.ahead.length >= 20, `${slow.ahead.length} samples`);
      const ahead = Math.max(...slow.ahead);
      console.log(`upload ahead ${on}: ${(ahead / MiB).toFixed(2)} MiB`);
      // What the client can write ahead of what the Worker read is what the kernel's socket
      // buffers hold plus what the server buffers. On one thread the loop reads the socket
      // itself; on several the front reads it and hands the body to the loop a chunk at a
      // time, so neither may hold more than a few chunks: the bytes ahead are those of one
      // thread plus 4 MiB. Of that allowance the measurement takes up to 1.4 MiB (the ledger
      // hears of the Worker's reads per MiB, and a sample 50 ms late is 0.4 MiB behind at
      // 8 MiB a second), which leaves a few of the front's chunks. A side that buffered
      // tens of MiB would also break the absolute bound, about three times the 6 MiB that
      // the kernel's buffers of a connection over loopback take up on one thread on Linux.
      assert(ahead < 20 * MiB, `the client wrote ${ahead} bytes ahead`);
      const what = 'the bytes the client wrote ahead of a slow upload';
      if (!several) {
        observed.set(what, ahead);
      } else if (observed.has(what)) {
        const one = observed.get(what);
        assert(
          ahead <= one + 4 * MiB,
          `the client wrote ${ahead} bytes ahead, ${one} on one thread`
        );
      }
      if (RSS) assert(slow.rssGrowth < 150 * MiB, `RSS grew ${slow.rssGrowth}`);
    });

    test(
      `a thread that reads a body without a pause serves what other threads ask of it ${on}`,
      { skip: !several && 'one thread has no other' },
      async () => {
        const { server } = shared;
        const threads = await threadsOf(server);
        // The first thread reads 40 MiB in five seconds and runs JavaScript whenever it does
        // not read. The front has the next chunk ready before the thread asks for it, so the
        // thread always has something to do that it need not wait for.
        const size = 40 * MiB;
        const busy = await busyUpload(server, threads.first, {
          size,
          work: 125,
        });
        assert.strictEqual(busy.front, threads.first);
        await scheduler.wait(500);
        // Another thread asks the first for a value of the ledger, which lives there, and is
        // answered long before the upload ends.
        const asked = Date.now();
        await get(server.http, '/ledger?key=nothing', away(threads));
        const took = Date.now() - asked;
        assert(took < 2500, `the ledger answered after ${took} ms`);
        const { consumed } = await within(busy.done, 30_000, 'the upload');
        assert.strictEqual(consumed, size);
      }
    );

    test(`a large download streams to a slow client with bounded production and memory ${on}`, async () => {
      const { server } = shared;
      const slow = await download(server, 128 * MiB, 12);
      const ahead = Math.max(...slow.ahead);
      assert(ahead < 48 * MiB, `the Worker made ${ahead} bytes ahead`);
      if (RSS) assert(slow.rssGrowth < 150 * MiB, `RSS grew ${slow.rssGrowth}`);
      // A fast client gets every byte.
      await download(server, 128 * MiB);
    });

    test(`the response head comes before the request body ends and both bodies flow at once ${on}`, async () => {
      const req = http.request(
        options(shared.server.http, '/echo', { method: 'POST' })
      );
      const res = await new Promise((resolve, reject) => {
        req.on('error', reject);
        req.on('response', resolve);
        req.flushHeaders();
      });
      assert.strictEqual(res.statusCode, 200);
      const received = [];
      let size = 0;
      let arrived;
      res.on('data', (chunk) => {
        received.push(chunk);
        size += chunk.length;
        arrived?.();
      });
      const ended = once(res, 'end');
      const sent = [];
      let total = 0;
      for (let i = 0; i < 100; i++) {
        const chunk = Buffer.from(`${i}:${'x'.repeat(1000)}\n`);
        sent.push(chunk);
        total += chunk.length;
        req.write(chunk);
        // The echo of this chunk arrives before the next is written.
        while (size < total) {
          await within(
            new Promise((resolve) => (arrived = resolve)),
            10_000,
            `the echo of chunk ${i}`
          );
        }
      }
      req.end();
      await within(ended, 10_000, 'the end of the echo');
      assert(Buffer.concat(received).equals(Buffer.concat(sent)));
    });

    test(`Expect: 100-continue is answered when the Worker reads the body ${on}`, async () => {
      const { server } = shared;
      const head = (path) =>
        `POST ${path} HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\n` +
        'Content-Length: 11\r\n\r\n';

      // The Worker reads the body before it answers: the client is told to send it.
      const reading = await connect(server.http.port);
      try {
        const incoming = new Incoming(reading);
        reading.write(head('/read'));
        assert.strictEqual((await incoming.head()).status, 100);
        reading.write('hello world');
        const res = await incoming.response();
        assert.strictEqual(res.status, 200);
        assert.strictEqual(res.body.toString(), 'hello world');
      } finally {
        reading.destroy();
      }

      // The Worker answers without reading it: no 100, and the connection closes after the
      // response, the body never sent.
      const ignoring = await connect(server.http.port);
      try {
        const incoming = new Incoming(ignoring);
        ignoring.write(head('/ignore'));
        const res = await incoming.response();
        assert.strictEqual(res.status, 200);
        assert.match(JSON.parse(res.body).front, UUID);
        assert.strictEqual((await incoming.rest()).length, 0);
      } finally {
        ignoring.destroy();
      }
    });

    // -------------------------------------------------------------------------------
    // Cancellation and failure

    test(
      `a client that resets before the head aborts request.signal ${on}`,
      {
        skip: !SEES_RESETS && 'Windows does not notice the reset',
      },
      async () => {
        const { server } = shared;
        const marker = unique('abort');
        const socket = await connect(server.http.port);
        await write(
          socket,
          `GET /wait?marker=${marker} HTTP/1.1\r\nHost: localhost\r\n\r\n`
        );
        const front = await ledgerValue(server, `arrived:${marker}`);
        socket.resetAndDestroy();
        await notesOf(server, front, marker, (notes) =>
          notes.includes('aborted')
        );
        await until(
          () => server.log.includes(`front-test: ${marker} aborted`),
          5000,
          'the log line of the abort'
        );
      }
    );

    test(
      `a queued request whose client resets never reaches a Worker ${on}`,
      {
        skip:
          (!several && 'one thread has no queue') ||
          (!SEES_RESETS && 'Windows does not notice the reset'),
      },
      async () => {
        const { server } = shared;
        const { all } = await threadsOf(server);
        const spinners = [];
        for (let i = 0; i < threads; i++) {
          spinners.push(await spin(server, 1500));
        }
        assert.strictEqual(new Set(spinners.map((s) => s.front)).size, threads);
        const reset = unique('reset');
        const kept = unique('kept');
        const sockets = [];
        for (const marker of [reset, kept]) {
          const socket = await connect(server.http.port);
          sockets.push(socket);
          await write(
            socket,
            `GET /?marker=${marker} HTTP/1.1\r\nHost: localhost\r\n\r\n`
          );
        }
        const incoming = new Incoming(sockets[1]);
        // The front reads both requests and queues them.
        await scheduler.wait(100);
        sockets[0].resetAndDestroy();
        const resetAt = Date.now();
        const res = await within(
          incoming.response(),
          20_000,
          'the kept request'
        );
        sockets[1].destroy();
        assert.strictEqual(res.status, 200);
        const spun = await Promise.all(spinners.map((s) => s.done));
        assert(resetAt < Math.min(...spun.map((s) => s.end)));
        const seen = [];
        for (const front of all) {
          seen.push(...(await get(server.http, '/seen', front)).seen);
        }
        assert(seen.includes(kept), 'the kept request was not served');
        assert(!seen.includes(reset), 'the reset request was served');
      }
    );

    test(`a client that resets in the middle of the bodies cancels the Worker's streams ${on}`, async () => {
      const { server } = shared;
      const marker = unique('mid-body');
      const socket = await connect(server.http.port);
      const incoming = new Incoming(socket);
      await write(
        socket,
        `POST /stream-echo?marker=${marker} HTTP/1.1\r\nHost: localhost\r\n` +
          'Transfer-Encoding: chunked\r\n\r\n10\r\nthe first chunk\n\r\n'
      );
      const head = await incoming.head();
      assert.strictEqual(head.status, 200);
      const front = head.headers['x-front'];
      assert.strictEqual(await incoming.until('\r\n'), '10\r\n');
      assert.strictEqual(
        (await incoming.bytes(16)).toString(),
        'the first chunk\n'
      );
      socket.resetAndDestroy();
      // The Worker learns at once, in one of two ways. Its call is dropped: its response body
      // is cancelled, and then `request.signal` aborts. That is what one thread always does
      // where the system reports the hang-up, which the thread sees before anything else. Or
      // the read of the request body that it waits for fails, and the call ends by that
      // failure, so nothing is left to abort. That is how one thread learns of it on Windows,
      // where only the read tells. On several threads the connection's end reaches the loop
      // both as the end of the body it reads and as the drop of the call, and now and then
      // the body comes first.
      const cancelled = ['response cancelled: undefined', 'signal aborted'];
      const failed = ['request body failed: Network connection lost.'];
      const ended = (notes) =>
        notes.includes('signal aborted') || notes.includes(failed[0]);
      await notesOf(server, front, marker, ended, 5000);
      // Nothing more follows either.
      await scheduler.wait(300);
      const notes = await notesOf(server, front, marker, ended);
      if (several) {
        assert.deepStrictEqual(
          notes,
          notes[0] === failed[0] ? failed : cancelled
        );
      } else {
        assert.deepStrictEqual(notes, SEES_RESETS ? cancelled : failed);
      }
    });

    test(`a Worker that throws before the head gets a bare 500 ${on}`, async () => {
      const { server } = shared;
      const marker = unique('throw');
      const res = await send(server.http, `/throw?marker=${marker}`);
      assert.strictEqual(res.status, 500);
      assert.strictEqual(res.body.toString(), 'Internal Server Error');
      assert.strictEqual(res.headers.connection, 'close');
      await until(
        () =>
          server.log.includes(`front-test: ${marker} threw before the head`),
        5000,
        'the log line of the exception'
      );
      const line = server.log
        .split('\n')
        .find((line) => line.includes(`${marker} threw`));
      assert.match(line, /Uncaught exception/);
    });

    for (const framing of ['length', 'chunked']) {
      test(`a Worker that fails after the head leaves a truncated ${framing} response ${on}`, async () => {
        const { server } = shared;
        const marker = unique(framing);
        const failed = await partial(
          server.http,
          `/fail-after-head?framing=${framing}&marker=${marker}`
        );
        await until(
          () =>
            server.log.includes(`front-test: ${marker} failed after the head`),
          5000,
          'the log line of the failure'
        );
        if (failed.status === null) {
          // On several threads the head now and then (about one request in a thousand)
          // does not reach the client before the body fails: the connection is then reset
          // before any byte of the response.
          assert(several, 'the connection was reset before the head');
          assert.strictEqual(failed.bytes, 0, 'bytes before the reset');
          return;
        }
        assert.strictEqual(failed.status, 200);
        if (framing === 'length') {
          assert.strictEqual(failed.headers['content-length'], '8192');
        } else {
          assert.strictEqual(failed.headers['transfer-encoding'], 'chunked');
        }
        assert(failed.received <= 4096, `${failed.received} bytes`);
        assert.strictEqual(failed.complete, false, 'the response completed');
      });
    }

    // -------------------------------------------------------------------------------
    // Upgrades and other protocols

    test(`a WebSocket echoes and closes from either end ${on}`, async () => {
      const { server } = shared;
      const thread = away(await threadsOf(server));
      const open = () => connect(server.http.port);

      const marker = unique('ws');
      const ws = await webSocket(open, `/ws?marker=${marker}`, thread, threads);
      assert.strictEqual(ws.headers['x-front'], thread);
      await echoMessages(ws);
      ws.close(4000, 'bye from the client');
      assert.deepStrictEqual(await ws.next(), {
        code: 4000,
        reason: 'bye from the client',
      });
      await within(ws.closed, 5000, 'the end of the connection');
      // A thread that got more than one attempt of the upgrade closed the others' at once.
      const clientClose = (note) => note.code !== 1006;
      const notes = await notesOf(server, thread, marker, (notes) =>
        notes.some(clientClose)
      );
      assert.deepStrictEqual(notes.filter(clientClose), [
        { code: 4000, reason: 'bye from the client' },
      ]);

      const closing = await webSocket(
        open,
        `/ws?marker=${unique('ws')}`,
        thread,
        threads
      );
      closing.sendText('close-me');
      assert.deepStrictEqual(await closing.next(), {
        code: 4001,
        reason: 'bye from the worker',
      });
      closing.close(4001, '');
      await within(closing.closed, 5000, 'the end of the connection');
    });

    test(`a hibernatable WebSocket reaches its object from any thread ${on}`, async () => {
      // `threads-durable-test` has the object evicted while such a WebSocket stays open,
      // and woken by its next message; this case has two threads hold one each.
      const { server } = shared;
      const { first, replicas } = await threadsOf(server);
      const open = () => connect(server.http.port);
      const name = unique('room');
      const a = await webSocket(
        open,
        `/room?name=${name}`,
        replicas[0] ?? first,
        threads
      );
      a.sendText('one');
      const one = JSON.parse((await a.next()).text);
      // The room runs on its home thread, which is one of the server's.
      const room = one.isolate;
      assert([first, ...replicas].includes(room), `the room runs in ${room}`);
      assert.deepStrictEqual(one, {
        echo: 'one',
        isolate: room,
        sockets: 1,
      });
      const b = await webSocket(
        open,
        `/room?name=${name}`,
        replicas[1] ?? first,
        threads
      );
      b.sendText('two');
      assert.deepStrictEqual(JSON.parse((await b.next()).text), {
        echo: 'two',
        isolate: room,
        sockets: 2,
      });
      a.sendText('three');
      assert.deepStrictEqual(JSON.parse((await a.next()).text), {
        echo: 'three',
        isolate: room,
        sockets: 2,
      });
      for (const ws of [a, b]) {
        ws.close(1000, 'done');
        assert.deepStrictEqual(await ws.next(), {
          code: 1000,
          reason: 'done',
        });
        ws.destroy();
      }
    });

    test(`the connections of a tcp socket are connect() events on different threads ${on}`, async () => {
      const { server } = shared;
      const open = async () => {
        const socket = await connect(server.ports.tcp);
        return { socket, incoming: new Incoming(socket) };
      };
      // A connection whose handler runs keeps its thread from being the one the next
      // connection goes to, unless that thread is still awake from it and takes the next
      // itself: connections stay open until every thread serves one.
      const sessions = [];
      const fronts = new Set();
      try {
        for (let i = 0; i < TRIES && fronts.size < threads; i++) {
          const session = await tcpSession(open);
          sessions.push(session);
          fronts.add(session.front);
        }
      } finally {
        for (const session of sessions) await session.finish();
      }
      assert.strictEqual(fronts.size, threads, 'isolates that served');
    });

    test(`an HTTP CONNECT is tunneled to the connect() handler ${on}`, async () => {
      const { server } = shared;
      const open = async () => {
        const socket = await connect(server.http.port);
        const incoming = new Incoming(socket);
        socket.write(
          'CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n'
        );
        const head = await incoming.head();
        assert.strictEqual(head.status, 200);
        // What arrives past the head is the tunnel's.
        return { socket, incoming };
      };
      await (await tcpSession(open)).finish();
    });

    test(`the connections of a tcp socket with TLS are connect() events on different threads ${on}`, async () => {
      const { server } = shared;
      const open = async () => {
        const socket = await connect(server.ports['tcp-tls'], { tls: true });
        return { socket, incoming: new Incoming(socket) };
      };
      // As for the plain tcp socket, connections stay open until two threads serve one.
      const sessions = [];
      const fronts = new Set();
      try {
        for (let i = 0; i < TRIES && fronts.size < Math.min(threads, 2); i++) {
          const session = await tcpSession(open);
          sessions.push(session);
          fronts.add(session.front);
        }
      } finally {
        for (const session of sessions) await session.finish();
      }
      assert.strictEqual(
        fronts.size,
        Math.min(threads, 2),
        'isolates that served'
      );
    });

    test(`the connect() handler of a starttls socket upgrades its connection with startTls() ${on}`, async () => {
      const { server } = shared;
      let plain;
      const open = async () => {
        const socket = await connect(server.ports.starttls);
        try {
          const incoming = new Incoming(socket);
          plain = /^hello ([0-9a-f-]{36})\n$/.exec(await incoming.until('\n'));
          assert(plain !== null, 'the handler greets with its identity');
          socket.write('in plaintext\n');
          assert.strictEqual(await incoming.until('\n'), 'in plaintext\n');
          socket.write('starttls\n');
          assert.strictEqual(await incoming.until('\n'), 'go ahead\n');
          // Nothing but the handshake follows on the connection.
          const secure = tls.connect({
            socket,
            ca: CA,
            servername: 'localhost',
          });
          await once(secure, 'secureConnect');
          return { socket: secure, incoming: new Incoming(secure) };
        } catch (error) {
          socket.destroy();
          throw error;
        }
      };
      const session = await tcpSession(open);
      // The handler that greeted in plaintext is the one that serves over TLS.
      assert.strictEqual(session.front, plain[1]);
      await session.finish();
    });

    test(`an https socket serves requests and WebSockets ${on}`, async () => {
      const { server } = shared;
      const target = { port: server.ports.https, tls: true };
      await oneConnection(server, target);
      const thread = away(await threadsOf(server));
      const open = () => connect(server.ports.https, { tls: true });
      const ws = await webSocket(
        open,
        `/ws?marker=${unique('tls')}`,
        thread,
        threads
      );
      await echoMessages(ws);
      ws.close(1000, 'done');
      assert.deepStrictEqual(await ws.next(), { code: 1000, reason: 'done' });
      await within(ws.closed, 5000, 'the end of the connection');
    });

    test(`the http options of a socket apply as on one thread ${on}`, async () => {
      const { server } = shared;

      // Proxy headers, a cf blob and injected headers.
      const rewritten = await send({ port: server.ports.rewrite }, '/meta', {
        headers: {
          Host: 'example.com',
          'X-Forwarded-Proto': 'https',
          'X-Injected': 'by the client',
          'CF-Blob': JSON.stringify({ colo: 'TST' }),
        },
      });
      assert.strictEqual(rewritten.status, 200);
      assert.strictEqual(
        rewritten.headers['x-injected-response'],
        'by the socket'
      );
      const meta = JSON.parse(rewritten.body);
      assert.strictEqual(meta.url, 'https://example.com/meta');
      assert.strictEqual(meta.headers['x-injected'], 'by the socket');
      assert.deepStrictEqual(meta.cf, { colo: 'TST' });
      delete meta.front;
      observe(threads, 'a request on the rewriting socket', meta);

      // Without a cf blob header, the cf blob names the peer.
      const plain = await send(server.http, '/meta', {
        headers: { Host: 'example.com' },
      });
      const peer = JSON.parse(plain.body);
      assert.deepStrictEqual(peer.cf, {
        clientIp: `127.0.0.1:${plain.socket.localPort}`,
      });
      delete peer.front;
      observe(threads, 'a request on a plain socket', peer);

      // A proxy socket takes absolute URLs.
      const socket = await connect(server.ports.proxy);
      try {
        const incoming = new Incoming(socket);
        socket.write(
          'GET http://example.com/meta?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n'
        );
        const res = await incoming.response();
        assert.strictEqual(res.status, 200);
        const proxied = JSON.parse(res.body);
        assert.strictEqual(proxied.url, 'http://example.com/meta?x=1');
        delete proxied.front;
        observe(threads, 'a request on a proxy socket', proxied);
      } finally {
        socket.destroy();
      }
    });
  });
}

// =====================================================================================
// The cases with a server of their own

// Connects to `port` until a connection is refused, which is once the server stopped accepting;
// fails if it does not within `ms`.
async function refused(port, ms = 10_000) {
  const end = Date.now() + ms;
  for (;;) {
    const outcome = await new Promise((resolve) => {
      const socket = net.connect({ host: '127.0.0.1', port });
      socket.on('connect', () => {
        socket.destroy();
        resolve('connected');
      });
      socket.on('error', (error) => resolve(error.code));
    });
    if (outcome === 'ECONNREFUSED') return;
    assert(
      Date.now() < end,
      `connections were still ${outcome} after ${ms} ms`
    );
    await scheduler.wait(20);
  }
}

test(
  'a unix socket and an inherited socket serve on every thread',
  {
    skip: process.platform === 'win32' && 'Node.js passes no socket on Windows',
  },
  async () => {
    // Unix socket paths are short: the test's temporary directory may be too deep for one.
    let base = env.TEST_TMPDIR ?? tmpdir();
    if (base.length > 60) base = '/tmp';
    const directory = mkdtempSync(path.join(base, 'front-'));
    const socketPath = path.join(directory, 'front.sock');
    // The server gets its listening socket for `alt` from this process, as descriptor 4.
    const listening = net.createServer();
    await new Promise((resolve) => listening.listen(0, '127.0.0.1', resolve));
    const { port } = listening.address();
    const server = await serve({
      args: [`--socket-addr=http=unix:${socketPath}`, '--socket-fd=alt=4'],
      fds: [listening._handle.fd],
    });
    try {
      // This process only lends the socket: it accepts nothing from it.
      await new Promise((resolve) => listening.close(resolve));
      assert.strictEqual(server.ports.alt, port);
      // The inherited socket is the one the requests that hold threads go to.
      server.http = { port, threads: THREADS };
      await oneConnection(server, { socketPath, threads: THREADS });
      await oneConnection(server);
    } catch (error) {
      await server.kill();
      throw error;
    }
    assert.deepStrictEqual(await server.stop(), SIGTERM_EXIT);
  }
);

test('every socket gets one listen event, and a burst of requests sent as one is read is served by every thread', async () => {
  const burst = (port) =>
    Promise.all(
      Array.from({ length: 32 }, () =>
        get({ port, threads: THREADS }, '/sleep?ms=200')
      )
    );
  let first;
  const server = await serve({
    onListen: (event) => {
      if (event.socket === 'http') {
        first = burst(event.port);
        first.catch(() => {});
      }
    },
  });
  const listens = () => server.events.filter((e) => e.event === 'listen');
  try {
    assert.deepStrictEqual(
      listens()
        .map((e) => e.socket)
        .sort(),
      [...SOCKETS].sort()
    );
    for (const { socket, port } of listens()) {
      assert(Number.isInteger(port) && port > 0, `port ${port}`);
      if (socket !== 'udp') (await connect(port)).destroy();
    }
    const fronts = new Set((await first).map((answer) => answer.front));
    for (let i = 0; i < ROUNDS && fronts.size < THREADS; i++) {
      for (const { front } of await burst(server.ports.http)) fronts.add(front);
    }
    assert.strictEqual(fronts.size, THREADS, 'isolates that served the bursts');
  } catch (error) {
    await server.kill();
    throw error;
  }
  assert.deepStrictEqual(await server.stop(), SIGTERM_EXIT);
  assert.strictEqual(listens().length, SOCKETS.length, 'listen events');
});

for (const threads of [THREADS, 1]) {
  const on = `on ${threads} thread${threads > 1 ? 's' : ''}`;
  const several = threads > 1;
  const skip = !SIGTERM_DRAINS && 'SIGTERM does not drain on Windows';

  test(
    `SIGTERM finishes the requests in flight${several ? ' and queued' : ''}, closes idle connections and exits ${on}`,
    { skip },
    async () => {
      const server = await serve({ threads });
      try {
        // The first thread's isolate: that thread handles SIGTERM.
        const first = several ? await firstThread(server) : undefined;
        // An idle keep-alive connection.
        const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
        const idle = await send(server.http, '/', { agent });
        let idleClosedAt;
        idle.socket.on('close', () => (idleClosedAt = Date.now()));
        // A request in flight on a thread.
        const inFlight = unique('in-flight');
        const answers = [
          send(
            server.http,
            // On several threads it is still in flight when the first thread's spin below ends.
            `/sleep?marker=${inFlight}&announce&ms=${several ? 5000 : 1500}`
          ),
        ];
        await ledgerValue(server, `arrived:${inFlight}`);
        // On several threads every thread is then kept busy, each until a time chosen here, so
        // that how long steering takes does not matter: the first thread until `firstUntil`,
        // and every other (a busy thread takes nothing, so each spin goes to another) 3000 ms
        // longer. The first thread handles SIGTERM only once its spin ends.
        const spinners = [];
        let firstUntil;
        if (several) {
          firstUntil = Date.now() + 3000;
          spinners.push(await spin(server, { until: firstUntil }, first));
          for (let i = 1; i < threads; i++) {
            spinners.push(await spin(server, { until: firstUntil + 3000 }));
          }
          assert.strictEqual(spinners[0].front, first);
          assert.strictEqual(
            new Set(spinners.map((s) => s.front)).size,
            threads
          );
          // Requests that keep the thread that takes them in JavaScript for 300 ms each, on
          // connections of their own, sent 50 ms apart so that they are queued in the order
          // sent. Once its spin has ended the first thread serves them one after another, and
          // handles SIGTERM between two of them, while every other thread still spins: all but
          // the first few start after the drain began.
          for (let i = 0; i < 6; i++) {
            const socket = await connect(server.http.port);
            const incoming = new Incoming(socket);
            await write(
              socket,
              `GET /spin?ms=300&marker=${unique('queued')} HTTP/1.1\r\n` +
                'Host: localhost\r\n\r\n'
            );
            // With what arrived after the response, once the connection closed.
            const response = (async () => {
              const res = await incoming.response();
              return { ...res, rest: await incoming.rest() };
            })();
            response.finally(() => socket.destroy()).catch(() => {});
            answers.push(response);
            await scheduler.wait(50);
          }
        }
        const signaledAt = Date.now();
        server.child.kill('SIGTERM');
        // When the drain was seen to have begun, on several threads: a connection was refused.
        let refusedAt;
        if (several) {
          assert(
            signaledAt < firstUntil,
            `SIGTERM was sent ${signaledAt - firstUntil} ms after the first thread's spin ` +
              'ended: the test is too slow to show anything'
          );
          // One thread stops accepting but keeps its listening socket until it exits.
          await refused(server.http.port);
          refusedAt = Date.now();
        }
        const responses = await within(
          Promise.all(answers),
          20_000,
          'the answers'
        );
        const answeredAt = Date.now();
        for (const res of responses) assert.strictEqual(res.status, 200);
        assert.strictEqual(responses[0].headers.connection, 'close');
        const spun = await Promise.all(spinners.map((s) => s.done));
        for (const { end } of spun) {
          assert(signaledAt < end, 'a thread was free when SIGTERM came');
        }
        if (several) {
          const queued = responses.slice(1).map((res) => ({
            start: JSON.parse(res.body).start,
            connection: res.headers.connection,
            rest: res.rest.length,
          }));
          const starts = queued.map(({ start }) => start);
          for (const { start, rest } of queued) {
            assert(
              start >= spun[0].end,
              'a queued request started before the first thread was free'
            );
            // The drain closes the connection after the response.
            assert.strictEqual(rest, 0, 'bytes after the response');
          }
          // The requests that started after a connection was refused were still queued when
          // the drain began. The first thread may take a queued request before it handles
          // the signal; the head of such a request goes out before the drain.
          const late = queued.filter(({ start }) => start > refusedAt);
          assert(
            late.length >= 2,
            `${late.length} queued requests started after a connection was refused at ` +
              `${refusedAt}: they started at ${starts.join(', ')}`
          );
          // A response whose head goes out while the server drains says that the connection
          // closes after it. The front stops accepting a moment before it tells its
          // connections, so that holds for a request that started well after the refusal,
          // as the last one did.
          for (const { start, connection } of queued) {
            if (start > refusedAt + 200)
              assert.strictEqual(connection, 'close');
          }
          assert(
            Math.max(...starts) > refusedAt + 200,
            'no request started late'
          );
          for (const { end } of spun.slice(1)) {
            assert(end > refusedAt, 'a thread was free when the drain began');
          }
        }
        assert(idleClosedAt < answeredAt, 'the idle connection stayed open');
        agent.destroy();
        assert.deepStrictEqual(await server.exited(), [0, null]);
      } catch (error) {
        await server.kill();
        throw error;
      }
    }
  );

  test(
    `SIGTERM leaves an open WebSocket working until it closes ${on}`,
    { skip },
    async () => {
      const server = await serve({ threads });
      try {
        const thread = away(await threadsOf(server));
        const open = () => connect(server.http.port);
        const ws = await webSocket(
          open,
          `/ws?marker=${unique('ws')}`,
          thread,
          threads
        );
        ws.sendText('before');
        assert.deepStrictEqual(await ws.next(), { text: 'before' });
        server.child.kill('SIGTERM');
        if (several) {
          await refused(server.http.port);
        } else {
          // One thread says nothing when it begins to drain.
          await scheduler.wait(500);
        }
        ws.sendText('after');
        assert.deepStrictEqual(await ws.next(), { text: 'after' });
        assert.strictEqual(server.child.exitCode, null, 'the server exited');
        ws.close(1000, 'done');
        assert.deepStrictEqual(await ws.next(), { code: 1000, reason: 'done' });
        assert.deepStrictEqual(await server.exited(), [0, null]);
      } catch (error) {
        await server.kill();
        throw error;
      }
    }
  );

  test(
    `SIGTERM leaves an open tcp connection working until it closes ${on}`,
    { skip },
    async () => {
      const server = await serve({ threads });
      try {
        const socket = await connect(server.ports.tcp);
        const incoming = new Incoming(socket);
        assert.match(await incoming.until('\n'), /^hello [0-9a-f-]{36}\n$/);
        let sent = 0;
        const exchange = async (line) => {
          socket.write(line);
          sent += line.length;
          assert.strictEqual(await incoming.until('\n'), line);
        };
        for (const line of ['before 1\n', 'before 2\n', 'before 3\n']) {
          await exchange(line);
        }
        server.child.kill('SIGTERM');
        if (several) {
          // The accept loops stop together; one of the tcp socket's would make a connect()
          // event of each connection that finds it still accepting.
          await refused(server.http.port);
        } else {
          // One thread says nothing when it begins to drain.
          await scheduler.wait(500);
        }
        await scheduler.wait(500);
        for (const line of ['after 1\n', 'after 2\n', 'after 3\n']) {
          await exchange(line);
        }
        assert.strictEqual(server.child.exitCode, null, 'the server exited');
        socket.end();
        assert.strictEqual(await incoming.until('\n'), `bye ${sent}\n`);
        assert.strictEqual((await incoming.rest()).length, 0);
        socket.destroy();
        assert.deepStrictEqual(await server.exited(), [0, null]);
        const failures = server.log
          .split('\n')
          .filter(
            (line) =>
              line.includes('connect() handler threw') ||
              (line.includes('error:') && /TCP|connect/.test(line))
          );
        assert.deepStrictEqual(failures, [], 'failures the server logged');
      } catch (error) {
        await server.kill();
        throw error;
      }
    }
  );

  test(
    `SIGTERM serves the tcp connections that are still queued ${on}`,
    { skip: skip || (!several && 'one thread has no queue') },
    async () => {
      const server = await serve({ threads });
      try {
        // The first thread's isolate: that thread handles SIGTERM.
        const first = await firstThread(server);
        // Every thread is kept busy, each until a time chosen here: the first thread until
        // `firstUntil`, every other 3000 ms longer. The first thread handles SIGTERM only
        // once its spin ends.
        const firstUntil = Date.now() + 3000;
        const spinners = [await spin(server, { until: firstUntil }, first)];
        for (let i = 1; i < threads; i++) {
          spinners.push(await spin(server, { until: firstUntil + 3000 }));
        }
        assert.strictEqual(spinners[0].front, first);
        assert.strictEqual(new Set(spinners.map((s) => s.front)).size, threads);
        // Requests that keep the thread that takes them in JavaScript for 300 ms each, sent
        // 50 ms apart so that they are queued in the order sent, ahead of the tcp
        // connections: once its spin has ended the first thread serves them one after
        // another, and handles SIGTERM between two of them.
        const answers = [];
        for (let i = 0; i < 4; i++) {
          const socket = await connect(server.http.port);
          const incoming = new Incoming(socket);
          await write(
            socket,
            `GET /spin?ms=300&marker=${unique('queued')} HTTP/1.1\r\n` +
              'Host: localhost\r\n\r\n'
          );
          const response = incoming.response();
          response.finally(() => socket.destroy()).catch(() => {});
          answers.push(response);
          await scheduler.wait(50);
        }
        // Connections of the tcp socket, each of which sends a line before anything arrived;
        // `greeted` resolves with when the greeting arrived.
        const connections = [];
        for (let i = 0; i < 3; i++) {
          const socket = await connect(server.ports.tcp);
          const incoming = new Incoming(socket);
          const line = `queued ${i}\n`;
          socket.write(line);
          const greeted = incoming.until('\n').then((hello) => {
            assert.match(hello, /^hello [0-9a-f-]{36}\n$/);
            return Date.now();
          });
          greeted.catch(() => {});
          connections.push({ socket, incoming, line, greeted });
        }
        const signaledAt = Date.now();
        server.child.kill('SIGTERM');
        assert(
          signaledAt < firstUntil,
          `SIGTERM was sent ${signaledAt - firstUntil} ms after the first thread's spin ` +
            'ended: the test is too slow to show anything'
        );
        // The accept loops stop together: see the case above.
        await refused(server.http.port);
        const refusedAt = Date.now();
        const greetings = await within(
          Promise.all(connections.map(({ greeted }) => greeted)),
          20_000,
          'the greetings'
        );
        const responses = await within(
          Promise.all(answers),
          20_000,
          'the answers'
        );
        for (const res of responses) assert.strictEqual(res.status, 200);
        const spun = await Promise.all(spinners.map((s) => s.done));
        // Every HTTP connection has ended, the spins' too, and nothing is queued any more: a
        // front that took itself for drained now would be gone by the time the connections
        // are used again.
        await scheduler.wait(500);
        assert.strictEqual(server.child.exitCode, null, 'the server exited');
        for (const { socket, incoming, line } of connections) {
          try {
            assert.strictEqual(await incoming.until('\n'), line);
            socket.write('after\n');
            assert.strictEqual(await incoming.until('\n'), 'after\n');
            socket.end();
            const bytes = line.length + 'after\n'.length;
            assert.strictEqual(await incoming.until('\n'), `bye ${bytes}\n`);
            assert.strictEqual((await incoming.rest()).length, 0);
          } finally {
            socket.destroy();
          }
        }
        console.log(
          `queued tcp connections: refused at +${refusedAt - signaledAt} ms, greeted at ` +
            `${greetings.map((at) => `+${at - signaledAt}`).join(', ')} ms after SIGTERM`
        );
        // Each was still queued when the drain was seen to have begun.
        for (const at of greetings) {
          assert(
            at > refusedAt,
            `a tcp connection was greeted ${refusedAt - at} ms before a connection was refused`
          );
        }
        for (const { end } of spun.slice(1)) {
          assert(end > refusedAt, 'a thread was free when the drain began');
        }
        assert.deepStrictEqual(await server.exited(), [0, null]);
        assert(
          !server.log.includes('connect() handler threw'),
          'a connect() handler threw'
        );
      } catch (error) {
        await server.kill();
        throw error;
      }
    }
  );
}

// The number of `connect()` events the isolates `all` have had, together.
async function connectsOf(server, all) {
  let connects = 0;
  for (const thread of all) {
    connects += (await get(server.http, '/connects', thread)).connects;
  }
  return connects;
}

test(
  `a queued tcp connection whose client resets is closed and never reaches a Worker on ${THREADS} threads`,
  { skip: !SEES_RESETS && 'Windows does not notice the reset' },
  () =>
    withServer({}, async (server) => {
      const linux = process.platform === 'linux';
      const { all } = await threadsOf(server);
      const connectsBefore = await connectsOf(server, all);
      // Every thread spins until `T`, so that every connection below stays queued.
      const T = Date.now() + 8000;
      const spinners = [];
      for (let i = 0; i < THREADS; i++) {
        spinners.push(await spin(server, { until: T }));
      }
      assert.strictEqual(new Set(spinners.map((s) => s.front)).size, THREADS);
      const before = linux ? descriptors(server) : 0;
      for (let i = 0; i < 100; i++) {
        const socket = await connect(server.ports.tcp);
        await write(socket, `reset ${i}\n`);
        socket.resetAndDestroy();
      }
      // The front notices each hang-up when the system reports it.
      if (linux) {
        const end = Date.now() + 3000;
        let now = descriptors(server);
        while (now > before + 8 && Date.now() < end) {
          await scheduler.wait(20);
          now = descriptors(server);
        }
        console.log(
          `descriptors with 100 reset tcp connections: ${now}, ${before} before`
        );
        assert(
          now <= before + 8,
          `the server has ${now} descriptors open, ${before} before the connections`
        );
      } else {
        await scheduler.wait(1000);
      }
      assert(
        Date.now() < T,
        `the descriptors settled ${Date.now() - T} ms after the spins ended: the test is ` +
          'too slow to show anything'
      );
      await Promise.all(spinners.map((s) => s.done));
      assert.strictEqual(
        await connectsOf(server, all),
        connectsBefore,
        'connect() events'
      );
      const open = async () => {
        const socket = await connect(server.ports.tcp);
        return { socket, incoming: new Incoming(socket) };
      };
      await (await tcpSession(open)).finish();
    })
);

test(`with every thread busy the front holds no more than its queue's limit on ${THREADS} threads`, () =>
  withServer({ args: ['--front-queue=8'] }, async (server) => {
    const linux = process.platform === 'linux';
    // Every thread spins until `T`, so that nothing queued is taken before then.
    const T = Date.now() + 6000;
    const spinners = [];
    for (let i = 0; i < THREADS; i++) {
      spinners.push(await spin(server, { until: T }));
    }
    assert.strictEqual(new Set(spinners.map((s) => s.front)).size, THREADS);
    const before = linux ? descriptors(server) : 0;
    const rssBefore = RSS ? server.rss() : 0;
    // Connections of three pipelined requests each, few enough for the listen backlog of
    // any system to hold those that are not accepted. A client's connection is open once the
    // system has completed its handshake, accepted or not, so nothing is waited for.
    const connections = [];
    for (let c = 0; c < 60; c++) {
      const socket = net.connect({ host: '127.0.0.1', port: server.http.port });
      const incoming = new Incoming(socket);
      const markers = [0, 1, 2].map((k) => `${c}-${k}`);
      socket.write(
        markers
          .map(
            (marker) =>
              `GET /?marker=${marker} HTTP/1.1\r\nHost: localhost\r\n\r\n`
          )
          .join('')
      );
      connections.push({ socket, incoming, markers });
      await scheduler.wait(5);
    }
    await scheduler.wait(500);
    assert(
      Date.now() < T,
      `the connections were made ${Date.now() - T} ms after the spins ended: the test is ` +
        'too slow to show anything'
    );
    // An accepted connection costs two descriptors: its socket and the one that watches it
    // for a hang-up. Past the limit's 8 the front may accept a few connections for one free
    // place; 60 connections would cost 120.
    if (linux) {
      const growth = descriptors(server) - before;
      console.log(`descriptors with 60 connections held back: +${growth}`);
      assert(
        growth <= 2 * (8 + 8),
        `the server opened ${growth} descriptors for the connections`
      );
    }
    if (RSS) {
      const growth = server.rss() - rssBefore;
      assert(growth < 64 * MiB, `RSS grew ${growth}`);
    }
    await Promise.all(spinners.map((s) => s.done));
    // Every request is answered, those of a connection in the order they were sent.
    await within(
      Promise.all(
        connections.map(async ({ incoming, markers }) => {
          for (const marker of markers) {
            const res = await incoming.response();
            assert.strictEqual(res.status, 200, res.body.toString());
            assert.strictEqual(JSON.parse(res.body).marker, marker);
          }
        })
      ),
      20_000,
      'the responses'
    );
    for (const { socket } of connections) socket.destroy();
    if (linux) {
      await until(
        () => descriptors(server) <= before + 8,
        3000,
        `the descriptors returning to ${before}`
      );
    }
  }));

for (const front of [1, 4]) {
  test(`--front-threads=${front} serves on every thread`, () =>
    withServer({ args: [`--front-threads=${front}`] }, async (server) => {
      await oneConnection(server);
      const body = pattern(0, 32 * MiB);
      const res = await send(server.http, '/echo', { method: 'POST', body });
      assert.strictEqual(res.status, 200);
      assert(res.body.equals(body), 'the echo differs');
    }));
}

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

test('--front-threads without several threads is a usage error', async () => {
  for (const args of [
    ['--front-threads=2'],
    ['--threads=1', '--front-threads=2'],
  ]) {
    const { code, signal, stderr } = await serveToExit([
      env.WORKERD_FRONT_CONFIG,
      ...args,
    ]);
    assert.deepStrictEqual([code, signal], [1, null], stderr);
    assert.match(
      stderr,
      /--front-threads: There is a front only with more than one thread/
    );
  }
});
