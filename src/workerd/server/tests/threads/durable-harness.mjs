// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What the drivers of `durable-config.capnp` share: a server of `workerd serve --threads`, and
// requests that the thread a test chooses serves.
//
// Whichever thread takes a request serves it, whatever connection it came on, so the tests learn
// the threads by the identities of their isolates and have the thread they choose serve what they
// test: a request names the isolate it is for (`x-thread`), any other isolate refuses it before
// doing anything with it (409), and the test sends it again until the one it names serves it
// (`steer`). Every Durable Object reports the identity of the isolate it runs in, which is that
// of its home thread's copy of the Worker, so the same identities say where an object lives.
import { spawn } from 'node:child_process';
import { randomBytes, randomUUID } from 'node:crypto';
import { mkdtempSync, readdirSync } from 'node:fs';
import http from 'node:http';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { env } from 'node:process';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WORKERD_DURABLE_CONFIG !== undefined,
  'You must set the WORKERD_DURABLE_CONFIG environment variable.'
);

export const THREADS = 4;
export const UUID = /^[0-9a-f-]{36}$/;

// Node's SIGTERM is TerminateProcess on Windows: the server neither drains nor exits by itself
// there.
export const SIGTERM_DRAINS = {
  skip: process.platform === 'win32' && 'SIGTERM does not drain on Windows',
};
// How the server ends on SIGTERM, as `[code, signal]`.
export const SIGTERM_EXIT =
  process.platform === 'win32' ? [null, 'SIGTERM'] : [0, null];

// Starts `workerd serve` on `threads` threads, with the objects in `directory`, or in a new
// directory if it is undefined. `log` is what the server has written so far; `pending` is how
// many of the requests `steer` sent are still unanswered.
export async function serve(threads, { directory } = {}) {
  directory ??= mkdtempSync(path.join(env.TEST_TMPDIR ?? tmpdir(), 'objects-'));
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      env.WORKERD_DURABLE_CONFIG,
      '--experimental',
      ...(threads > 1 ? [`--threads=${threads}`] : []),
      '--control-fd=3',
      `--directory-path=objects=${directory}`,
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe'] }
  );
  const server = {
    child,
    threads,
    directory,
    log: '',
    port: undefined,
    pending: 0,
  };
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
  server.stop = () => {
    child.kill('SIGTERM');
    return server.exited();
  };
  // Resolves once no more than `n` of the requests `steer` sent are unanswered.
  server.quiet = async (n) => {
    for (let tries = 0; server.pending > n; tries++) {
      assert(tries < TRIES, `${server.pending} requests are still unanswered`);
      await scheduler.wait(10);
    }
  };

  const control = child.stdio[3];
  control.setEncoding('utf8');
  let events = '';
  for await (const chunk of control) {
    events += chunk;
    if (events.includes('\n')) break;
  }
  assert(
    events.includes('\n'),
    `the server sent no listen event: ${server.log}`
  );
  const event = JSON.parse(events.split('\n')[0]);
  assert.strictEqual(event.event, 'listen', server.log);
  server.port = event.port;
  return server;
}

// Fails if the server logged that it leaked something as it ended, or that it crashed.
export function assertCleanEnd(server) {
  assert.doesNotMatch(
    server.log,
    /a service leaked|fatal runtime error|internal bug/
  );
}

// Runs `body` with a server on `threads` threads (`more` as for `serve`), which then stops on
// SIGTERM and must end cleanly.
export async function withServer(threads, body, more) {
  const server = await serve(threads, more);
  try {
    await body(server);
  } catch (error) {
    server.child.kill('SIGKILL');
    await server.closed();
    throw error;
  }
  const exit = await server.stop();
  assertCleanEnd(server);
  assert.deepStrictEqual(exit, SIGTERM_EXIT);
}

// How many times a test asks before it gives up on what it waits for.
export const TRIES = 200;

// How many rounds `steer` sends before it gives up.
const ROUNDS = 8;

// Sends a request until the isolate whose identity is `thread` serves it, or once to any isolate
// if `thread` is undefined. `attempt(headers)` sends the request once with `headers` and
// resolves with what it got, or with undefined if another isolate refused it (409); anything
// else it did fails the request, whatever the other attempts did.
//
// The front wakes the sleeping thread with the fewest requests in flight for each request, and
// among equals the one that slept last, so one request at a time would keep going to the same warm
// thread. Steering fans out instead: a round sends several attempts a few milliseconds apart, and
// an isolate that is not the one named holds its refusal for a while, so that it has a request in
// flight and the next attempt goes to another thread. Each round sends twice as many, so that a
// thread with requests of its own in flight is reached too. The attempts share an `x-attempt`, so
// the isolate named acts on one of them only, and refuses the others.
export async function steer(server, thread, attempt) {
  const send = (headers) => {
    const sent = attempt(headers);
    server.pending++;
    const done = () => server.pending--;
    sent.then(done, done);
    return sent;
  };
  if (thread === undefined) {
    const answer = await send({});
    assert.notStrictEqual(answer, undefined, 'a request was refused');
    return answer;
  }
  const headers = { 'x-thread': thread, 'x-attempt': randomUUID() };
  for (let round = 1; round <= ROUNDS; round++) {
    const attempts = [];
    for (let i = 0; i < server.threads * 2 ** (round - 1); i++) {
      attempts.push(send(headers));
      await scheduler.wait(2);
    }
    const results = await Promise.allSettled(attempts);
    const failure = results.find((r) => r.status === 'rejected');
    if (failure !== undefined) throw failure.reason;
    const answers = results
      .map((r) => r.value)
      .filter((answer) => answer !== undefined);
    assert(answers.length <= 1, `isolate ${thread} acted on ${answers.length}`);
    if (answers.length === 1) return answers[0];
  }
  assert.fail(`isolate ${thread} served none of ${ROUNDS} rounds of requests`);
}

// The options of a request for `path` on a connection of its own.
export function options(server, path, more = {}) {
  return { agent: false, host: '127.0.0.1', port: server.port, path, ...more };
}

// The parsed JSON response to a GET of `path`, served by the isolate `thread` (any if
// undefined).
export function get(server, path, thread) {
  return steer(
    server,
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(options(server, path, { headers }), (res) => {
          let body = '';
          res.setEncoding('utf8');
          res.on('data', (chunk) => (body += chunk));
          res.on('error', reject);
          res.on('end', () => {
            try {
              if (res.statusCode === 409 && thread !== undefined) {
                resolve(undefined);
                return;
              }
              assert.strictEqual(res.statusCode, 200, `${path}: ${body}`);
              const answer = JSON.parse(body);
              if (thread !== undefined) {
                assert.strictEqual(answer.front, thread);
              }
              resolve(answer);
            } catch (error) {
              reject(error);
            }
          });
        });
        req.setTimeout(20_000, () => {
          req.destroy(new Error(`no response to ${path} in 20 seconds`));
        });
        req.on('error', reject);
        req.end();
      })
  );
}

// A request for `path` whose response head the isolate `thread` sent, with its body still to
// write: `{ req, res }`.
export function open(server, path, thread, more) {
  return steer(
    server,
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(options(server, path, { ...more, headers }));
        req.on('error', reject);
        req.on('response', (res) => {
          if (res.statusCode === 409) {
            res.resume();
            res.on('end', () => {
              req.destroy();
              resolve(undefined);
            });
          } else {
            resolve({ req, res });
          }
        });
        req.flushHeaders();
      })
  );
}

// The whole body of `res`.
export function body(res) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    res.on('data', (chunk) => chunks.push(chunk));
    res.on('error', reject);
    res.on('end', () => resolve(Buffer.concat(chunks)));
  });
}

// The isolates of `front`, one per thread, by identity. A round sends a request per thread at
// once, each answered after a while: a thread with one in flight is passed over for one with
// none, so a round usually finds every thread. No object is called.
export async function threadsOf(server) {
  const fronts = new Set();
  for (let round = 0; round < ROUNDS && fronts.size < server.threads; round++) {
    const answers = [];
    for (let i = 0; i < server.threads; i++) {
      answers.push(get(server, '/front?ms=250'));
      await scheduler.wait(2);
    }
    for (const { front } of await Promise.all(answers)) {
      assert.match(front, UUID);
      fronts.add(front);
    }
  }
  assert.strictEqual(fronts.size, server.threads, 'isolates that served');
  return [...fronts];
}

// Names of objects of `prefix`, at least `per` of them homed on each thread of `fronts`, by the
// identity of the thread: the objects are asked where they run (from the isolate `via`, or any
// if undefined) until each thread has enough. Every object runs in the isolate of a thread.
export async function homes(server, fronts, { prefix, per = 1, via } = {}) {
  const byHome = new Map(fronts.map((front) => [front, []]));
  const short = () => [...byHome.values()].some((names) => names.length < per);
  const BATCH = 8;
  for (let i = 0; i < 64 * per && short(); i += BATCH) {
    const names = Array.from({ length: BATCH }, (_, j) => `${prefix}-${i + j}`);
    const wheres = await Promise.all(
      names.map((name) => get(server, `/where?name=${name}`, via))
    );
    for (const [j, where] of wheres.entries()) {
      const at = byHome.get(where.isolate);
      assert(at !== undefined, `${names[j]} runs in ${where.isolate}`);
      if (at.length < per) at.push(names[j]);
    }
  }
  for (const [front, names] of byHome) {
    assert(names.length >= per, `${names.length} objects live on ${front}`);
  }
  return byHome;
}

// The isolates of `fronts` but `home`, or `home` alone if there are no others.
export function others(fronts, home) {
  const rest = fronts.filter((front) => front !== home);
  return rest.length > 0 ? rest : [home];
}

// The alarm databases of the namespace `counter` in `directory`: the names of its files that
// start with `metadata`, without SQLite's `-wal` and `-shm` files.
export function metadataFiles(directory) {
  return readdirSync(path.join(directory, 'counter'))
    .filter((file) => /^metadata/.test(file) && !/-(wal|shm)$/.test(file))
    .sort();
}

// A WebSocket opened with a request for `path` that the isolate `thread` served: just enough
// of the protocol for short text messages.
export function webSocket(server, path, thread) {
  return steer(
    server,
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(
          options(server, path, {
            headers: {
              ...headers,
              Connection: 'Upgrade',
              Upgrade: 'websocket',
              'Sec-WebSocket-Version': '13',
              'Sec-WebSocket-Key': randomBytes(16).toString('base64'),
            },
          })
        );
        req.on('error', reject);
        req.on('response', (res) => {
          res.resume();
          if (res.statusCode === 409) resolve(undefined);
          else reject(new Error(`no upgrade: ${res.statusCode}`));
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
          resolve({
            front: res.headers['x-front'],
            isClosed: () => closed,
            send: (text) => frame(1, Buffer.from(text)),
            // The next message, parsed; null once the socket closed.
            next: () =>
              new Promise((resolve) => {
                if (received.length > 0) resolve(JSON.parse(received.shift()));
                else if (closed) resolve(null);
                else
                  waiting.push((m) =>
                    resolve(m === null ? null : JSON.parse(m))
                  );
              }),
            // Closes the WebSocket, then the connection once the server answered.
            close: async () => {
              frame(8, Buffer.from([0x03, 0xe8]));
              for (let i = 0; i < 100 && !closed; i++) await scheduler.wait(10);
              socket.destroy();
            },
          });
        });
        req.end();
      })
  );
}
