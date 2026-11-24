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
import { randomBytes, randomUUID } from 'node:crypto';
import { once } from 'node:events';
import { mkdtempSync, readFileSync } from 'node:fs';
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
const SOCKETS = ['http', 'alt', 'tcp', 'https', 'rewrite', 'proxy'];

// Node's SIGTERM is TerminateProcess on Windows: the server neither drains nor exits by itself
// there.
const SIGTERM_DRAINS = process.platform !== 'win32';
// How the server ends on SIGTERM, as `[code, signal]`.
const SIGTERM_EXIT =
  process.platform === 'win32' ? [null, 'SIGTERM'] : [0, null];
// The cases on several threads reset connections and read the server's memory use in ways that
// have not been tried on Windows.
const SEVERAL_THREADS = process.platform !== 'win32';
// The resident memory of a process can be read on Linux.
const RSS = process.platform === 'linux';

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

// Sends a request until the isolate whose identity is `thread` serves it, or once to any isolate
// if `thread` is undefined, on a server of `threads` threads. `attempt(headers)` sends the
// request once with `headers` and resolves with what it got, or with undefined if another
// isolate refused it.
//
// The front wakes the sleeping thread with the fewest requests in flight for each request, and
// among equals the one that slept last, so one request at a time would keep going to the same warm
// thread. Steering fans out instead: a round sends several attempts a few milliseconds apart, and
// an isolate that is not the one named holds its refusal for a while, so that it has a request in
// flight and the next attempt goes to another thread. Each round sends twice as many, so that a
// thread with requests of its own in flight is reached too. Only the isolate named acts on the
// request, but it may get more than one attempt of a round: `discard(answer)` disposes of the
// answers past the first.
async function steer(thread, attempt, { threads = THREADS, discard } = {}) {
  if (thread === undefined) {
    const answer = await attempt({});
    assert.notStrictEqual(answer, undefined, 'a request was refused');
    return answer;
  }
  const headers = { 'x-thread': thread };
  for (let round = 1; round <= ROUNDS; round++) {
    const attempts = [];
    for (let i = 0; i < threads * 2 ** (round - 1); i++) {
      const sent = attempt(headers);
      // A failure is the round's only if no attempt was answered (`settled`).
      sent.catch(() => {});
      attempts.push(sent);
      await scheduler.wait(2);
    }
    const answers = (await settled(attempts)).filter(
      (answer) => answer !== undefined
    );
    for (const extra of answers.slice(1)) await discard?.(extra);
    if (answers.length > 0) return answers[0];
  }
  assert.fail(`isolate ${thread} served none of ${ROUNDS} rounds of requests`);
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
      if (res.status === 409 && thread !== undefined) return undefined;
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

// The isolates of `front`, one per thread, by identity: `first` is the first thread's, which has
// the Durable Objects; `replicas` are the others'. A round sends a request per thread at once,
// each answered after a while: a thread with one in flight is passed over for one with none, so
// a round usually finds every thread.
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

// A request to `target`, a TCP socket without TLS, that the isolate `thread` holds in flight for `ms` milliseconds (any
// isolate if undefined), once its head is here: `{ front, release }`, where `release()` ends it
// by hanging up.
function hold(target, thread, ms = 20_000) {
  return steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(
          options(target, `/hold?ms=${ms}`, { headers })
        );
        req.on('error', reject);
        req.on('response', (res) => {
          if (res.statusCode === 409) {
            res.resume();
            res.on('end', () => resolve(undefined));
            return;
          }
          assert.strictEqual(res.statusCode, 200);
          res.on('error', () => {});
          req.on('error', () => {});
          // A client that only half-closes is not gone while its request is in flight: the
          // release resets the connection.
          resolve({
            front: res.headers['x-front'],
            release: () => res.socket.resetAndDestroy(),
          });
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
    for (const { release } of holds) release();
  }
  assert.strictEqual(sockets.size, 1, 'connections the requests went over');
  assert.strictEqual(
    fronts.length,
    server.threads,
    'isolates that served one connection'
  );
  return fronts;
}

// Has a thread run JavaScript for `ms` milliseconds: the isolate `thread` (any if undefined)
// answers, and once its response head is here the thread spins in the body's first pull.
// Resolves then with `{ front, headAt, done }`; `done` resolves with the Worker's `start` and
// `end` of the spin and `endAt`, when the body ended here.
function spin(server, ms, thread) {
  return steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(
          options(server.http, `/spin?ms=${ms}`, { headers })
        );
        req.on('error', reject);
        req.on('response', (res) => {
          if (res.statusCode === 409) {
            res.resume();
            res.on('end', () => resolve(undefined));
            return;
          }
          const headAt = Date.now();
          let body = '';
          res.setEncoding('utf8');
          const done = new Promise((resolve, reject) => {
            res.on('data', (chunk) => (body += chunk));
            res.on('error', reject);
            res.on('end', () =>
              resolve({ ...JSON.parse(body), endAt: Date.now() })
            );
          });
          done.catch(() => {});
          resolve({ front: res.headers['x-front'], headAt, done });
        });
        req.end();
      }),
    { threads: server.threads }
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
      if (head.status === 409) {
        socket.destroy();
        return undefined;
      }
      assert.strictEqual(head.status, 101);
      return new WebSocketClient(socket, incoming, head.headers);
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
// the connection closed; `{ status: null }` if it closed before the head arrived.
function partial(target, path) {
  return new Promise((resolve, reject) => {
    const req = http.request(options(target, path));
    req.on('error', (error) => {
      if (error.code === 'ECONNRESET') resolve({ status: null });
      else reject(error);
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

// What the server's options made of requests on four sockets, by the number of threads: the
// cases on one thread run first, and those on four are compared with them.
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
  describe(
    on,
    {
      skip:
        several &&
        !SEVERAL_THREADS &&
        'the cases on several threads have not been tried on Windows',
    },
    () => {
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
          const spinners = [];
          for (const ms of [1000, 2000, 2200, 2400]) {
            spinners.push(await spin(server, ms));
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
            // The front reads the request and queues it long before the next is sent.
            await scheduler.wait(50);
          }
          const answers = await within(
            Promise.all(responses),
            20_000,
            'the queued requests'
          );
          const spun = await Promise.all(spinners.map((s) => s.done));
          // Every request was sent while every thread was busy.
          assert(lastSent < Math.min(...spun.map((s) => s.end)));
          const firstFree = spinners[0].front;
          for (const res of answers) {
            assert.strictEqual(res.status, 200);
            assert.strictEqual(JSON.parse(res.body).front, firstFree);
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

      test(
        `a large upload streams through with backpressure and bounded memory ${on}`,
        { skip: !several && 'two 512 MiB uploads cost too long to run twice' },
        async () => {
          const { server } = shared;
          const size = 256 * MiB;
          const fast = await upload(server, { size });
          assert.strictEqual(fast.consumed, size);
          if (RSS)
            assert(fast.rssGrowth < 150 * MiB, `RSS grew ${fast.rssGrowth}`);

          // The Worker reads the first 96 MiB at 48 MiB a second.
          const slow = await upload(server, {
            size,
            slow: 96 * MiB,
            rate: 48 * MiB,
          });
          assert.strictEqual(slow.consumed, size);
          assert(slow.ahead.length >= 5, `${slow.ahead.length} samples`);
          const ahead = Math.max(...slow.ahead);
          assert(ahead < 48 * MiB, `the client wrote ${ahead} bytes ahead`);
          if (RSS)
            assert(slow.rssGrowth < 150 * MiB, `RSS grew ${slow.rssGrowth}`);
        }
      );

      test(`a large download streams to a slow client with bounded production and memory ${on}`, async () => {
        const { server } = shared;
        const slow = await download(server, 128 * MiB, 12);
        const ahead = Math.max(...slow.ahead);
        assert(ahead < 48 * MiB, `the Worker made ${ahead} bytes ahead`);
        if (RSS)
          assert(slow.rssGrowth < 150 * MiB, `RSS grew ${slow.rssGrowth}`);
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

      test(`a client that resets before the head aborts request.signal ${on}`, async () => {
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
      });

      test(
        `a queued request whose client resets never reaches a Worker ${on}`,
        { skip: !several && 'one thread has no queue' },
        async () => {
          const { server } = shared;
          const { all } = await threadsOf(server);
          const spinners = [];
          for (let i = 0; i < threads; i++) {
            spinners.push(await spin(server, 1500));
          }
          assert.strictEqual(
            new Set(spinners.map((s) => s.front)).size,
            threads
          );
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
        // The Worker learns at once: its response body is cancelled (one thread), or its
        // request body fails, whichever the loop sees first (several threads, where the front
        // sees the reset).
        const notes = await notesOf(
          server,
          front,
          marker,
          (notes) =>
            notes.some(
              (note) =>
                note.startsWith('response cancelled') ||
                note.startsWith('request body failed')
            ),
          5000
        );
        console.log(`mid-body notes ${on}: ${JSON.stringify(notes)}`);
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
          const failed = await partial(
            shared.server.http,
            `/fail-after-head?framing=${framing}&marker=${unique(framing)}`
          );
          // The head may not have reached the connection before the body failed, on
          // several threads; then the connection closes without a response.
          if (failed.status === null && several) return;
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
        const ws = await webSocket(
          open,
          `/ws?marker=${marker}`,
          thread,
          threads
        );
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
        assert.deepStrictEqual(JSON.parse((await a.next()).text), {
          echo: 'one',
          isolate: first,
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
          isolate: first,
          sockets: 2,
        });
        a.sendText('three');
        assert.deepStrictEqual(JSON.parse((await a.next()).text), {
          echo: 'three',
          isolate: first,
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
    }
  );
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

test(
  'every socket gets one listen event, and a burst of requests sent as one is read is served by every thread',
  {
    skip:
      !SEVERAL_THREADS &&
      'the cases on several threads have not been tried on Windows',
  },
  async () => {
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
      for (const { port } of listens()) {
        assert(Number.isInteger(port) && port > 0, `port ${port}`);
        (await connect(port)).destroy();
      }
      const fronts = new Set((await first).map((answer) => answer.front));
      for (let i = 0; i < ROUNDS && fronts.size < THREADS; i++) {
        for (const { front } of await burst(server.ports.http))
          fronts.add(front);
      }
      assert.strictEqual(
        fronts.size,
        THREADS,
        'isolates that served the bursts'
      );
    } catch (error) {
      await server.kill();
      throw error;
    }
    assert.deepStrictEqual(await server.stop(), SIGTERM_EXIT);
    assert.strictEqual(listens().length, SOCKETS.length, 'listen events');
  }
);

for (const threads of [THREADS, 1]) {
  const on = `on ${threads} thread${threads > 1 ? 's' : ''}`;
  const several = threads > 1;
  const skip =
    (!SIGTERM_DRAINS && 'SIGTERM does not drain on Windows') ||
    (several &&
      !SEVERAL_THREADS &&
      'the cases on several threads have not been tried on Windows');

  test(
    `SIGTERM finishes the requests in flight${several ? ' and queued' : ''}, closes idle connections and exits ${on}`,
    { skip },
    async () => {
      const server = await serve({ threads });
      try {
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
            // On several threads it outlasts the spins below.
            `/sleep?marker=${inFlight}&announce&ms=${several ? 3000 : 1500}`
          ),
        ];
        await ledgerValue(server, `arrived:${inFlight}`);
        // On several threads every thread is then kept busy (a busy thread takes nothing, so
        // each spin goes to another), and requests wait in the queue. The first thread handles
        // SIGTERM once its spin ends.
        const spinners = [];
        if (several) {
          for (let i = 0; i < threads; i++)
            spinners.push(await spin(server, 2000));
          assert.strictEqual(
            new Set(spinners.map((s) => s.front)).size,
            threads
          );
          for (let i = 0; i < 3; i++) {
            answers.push(
              send(server.http, `/sleep?marker=${unique('queued')}&ms=500`)
            );
          }
          // The front reads the requests and queues them.
          await scheduler.wait(100);
        }
        const signaledAt = Date.now();
        server.child.kill('SIGTERM');
        if (several) {
          // One thread stops accepting but keeps its listening socket until it exits.
          await refused(server.http.port);
        }
        const responses = await within(
          Promise.all(answers),
          20_000,
          'the answers'
        );
        const answeredAt = Date.now();
        for (const res of responses) {
          assert.strictEqual(res.status, 200);
          assert.strictEqual(res.headers.connection, 'close');
        }
        const spun = await Promise.all(spinners.map((s) => s.done));
        for (const { end } of spun) {
          assert(signaledAt < end, 'a thread was free when SIGTERM came');
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
}

for (const front of [1, 4]) {
  test(
    `--front-threads=${front} serves on every thread`,
    {
      skip:
        !SEVERAL_THREADS &&
        'the cases on several threads have not been tried on Windows',
    },
    () =>
      withServer({ args: [`--front-threads=${front}`] }, async (server) => {
        await oneConnection(server);
        const body = pattern(0, 32 * MiB);
        const res = await send(server.http, '/echo', { method: 'POST', body });
        assert.strictEqual(res.status, 200);
        assert(res.body.equals(body), 'the echo differs');
      })
  );
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
