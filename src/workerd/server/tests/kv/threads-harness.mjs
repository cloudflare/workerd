// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// What the tests of a wrangler project's helper Workers under `workerd serve
// --threads` share: the server, a prelude for the project's Worker by which
// a request names the thread that serves it, and the requests themselves.
//
// Whichever loop thread takes a request serves it, so a test learns the
// threads by the identities of their isolates: the Worker's global state is
// per thread, and it reports a random identity it keeps there (`x-front`). A
// request names the isolate it is for (`x-thread`), any other isolate refuses
// it after a moment (409), and the test sends it again until the one it names
// serves it (`steer`). The attempts of one request share an `x-attempt`, so
// that the isolate named acts on one of them only.

import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import http from 'node:http';
import { join } from 'node:path';
import { env } from 'node:process';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';

export const THREADS = 4;
const CONTROL_FD = 3;

// The start of the project's Worker: the steering, and
//
//   GET /front?ms=     answers after `ms` milliseconds;
//   GET /spin-body?ms= sends its head, then keeps its thread in JavaScript
//                      for `ms` milliseconds as the body is pulled; the
//                      body is `{start, end}` of the spin, by `Date.now()`.
//
// Every other request goes to `route(request, env, url)`, which the feature's
// test defines after this.
export const PRELUDE = `
let isolate;
const attempts = new Set();
export default {
  async fetch(request, env) {
    isolate ??= crypto.randomUUID();
    const headers = { 'x-front': isolate };
    const wanted = request.headers.get('x-thread');
    const attempt = request.headers.get('x-attempt');
    if (wanted !== null &&
        (wanted !== isolate || (attempt !== null && attempts.has(attempt)))) {
      await scheduler.wait(100);
      return new Response(null, { status: 409, headers });
    }
    if (attempt !== null) attempts.add(attempt);
    const url = new URL(request.url);
    const ms = Number(url.searchParams.get('ms') ?? 0);
    if (url.pathname === '/front') {
      await scheduler.wait(ms);
      return new Response(null, { headers });
    }
    if (url.pathname === '/spin-body') {
      const body = new ReadableStream({
        pull(controller) {
          const start = Date.now();
          while (Date.now() < start + ms) {}
          const end = Date.now();
          controller.enqueue(new TextEncoder().encode(JSON.stringify({ start, end })));
          controller.close();
        },
      }, { highWaterMark: 0 });
      return new Response(body, { headers });
    }
    const response = await route(request, env, url);
    const answer = new Response(response.body, response);
    answer.headers.set('x-front', isolate);
    return answer;
  },
};
`;

// Runs `workerd serve --experimental` on `dir`'s wrangler.jsonc with
// `threads` loop threads until it listens. Fails if it exits first.
export function serve(dir, threads = THREADS) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'wrangler.jsonc'),
      '--experimental',
      `--threads=${threads}`,
      `--control-fd=${CONTROL_FD}`,
      '--socket-addr=http=127.0.0.1:0',
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe'] }
  );
  const server = { threads, log: '', port: undefined };
  for (const stream of [child.stdout, child.stderr]) {
    stream.setEncoding('utf8');
    stream.on('data', (data) => {
      server.log += data;
      process.stderr.write(data);
    });
  }
  const exited = new Promise((resolve) =>
    child.once('exit', (code, signal) => resolve([code, signal]))
  );
  // Stops the server on SIGTERM; resolves with how it ended, once it did.
  server.stop = () => (child.kill('SIGTERM'), exited);
  server.kill = () => (child.kill('SIGKILL'), exited);
  return new Promise((resolve, reject) => {
    let buffer = '';
    child.stdio[CONTROL_FD].on('data', (data) => {
      buffer += data;
      for (const line of buffer.split('\n').slice(0, -1)) {
        const event = JSON.parse(line);
        if (event.event === 'listen') {
          server.port = event.port;
          resolve(server);
        }
      }
      buffer = buffer.slice(buffer.lastIndexOf('\n') + 1);
    });
    child.once('error', reject);
    exited.then(([code, signal]) =>
      reject(new Error(`workerd exited (${code}, ${signal}): ${server.log}`))
    );
  });
}

// Runs `body` with a server of `dir`'s project, which then must stop
// cleanly on SIGTERM. The server is killed if `body` fails.
export async function withServer(dir, body) {
  const server = await serve(dir);
  try {
    await body(server);
  } catch (error) {
    await server.kill();
    throw error;
  }
  const exit = await server.stop();
  if (process.platform !== 'win32') assert.deepStrictEqual(exit, [0, null]);
  assert.doesNotMatch(
    server.log,
    /a service leaked|fatal runtime error|internal bug|session with loop thread/
  );
}

// How many rounds `steer` sends before it gives up.
const ROUNDS = 8;

// Sends a request until the isolate whose identity is `thread` serves it,
// or once to any isolate if `thread` is undefined. `attempt(headers)` sends
// it once with `headers`, and resolves with what it got, or with undefined
// if another isolate refused it.
//
// The front gives a request to the sleeping thread with the fewest requests
// in flight, so a round sends several attempts a few milliseconds apart, and
// each refusal keeps its thread busy for a while; each round sends twice as
// many, so that a thread with requests of its own in flight is reached too.
export async function steer(server, thread, attempt) {
  if (thread === undefined) {
    const answer = await attempt({});
    assert.notStrictEqual(answer, undefined, 'a request was refused');
    return answer;
  }
  const headers = { 'x-thread': thread, 'x-attempt': randomUUID() };
  for (let round = 1; round <= ROUNDS; round++) {
    const attempts = [];
    for (let i = 0; i < server.threads * 2 ** (round - 1); i++) {
      attempts.push(attempt(headers));
      await scheduler.wait(2);
    }
    const answers = (await Promise.all(attempts)).filter(
      (answer) => answer !== undefined
    );
    assert(answers.length <= 1, `isolate ${thread} acted on ${answers.length}`);
    if (answers.length === 1) return answers[0];
  }
  assert.fail(`isolate ${thread} served none of ${ROUNDS} rounds of requests`);
}

// A request with `method` for `path` (and `body`, a string or Buffer, if
// given), served by the isolate `thread` (any if undefined): `{status,
// front, body}`, the body a Buffer. Any status but 409 is the answer.
export function request(server, { method = 'GET', path, body, thread }) {
  return steer(
    server,
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(
          {
            agent: false,
            host: '127.0.0.1',
            port: server.port,
            method,
            path,
            headers,
          },
          (res) => {
            const chunks = [];
            res.on('data', (chunk) => chunks.push(chunk));
            res.on('error', reject);
            res.on('end', () => {
              const front = res.headers['x-front'];
              if (res.statusCode === 409 && thread !== undefined) {
                resolve(undefined);
                return;
              }
              if (thread !== undefined && front !== thread) {
                reject(new Error(`${path} was served by ${front}`));
                return;
              }
              resolve({
                status: res.statusCode,
                front,
                body: Buffer.concat(chunks),
              });
            });
          }
        );
        req.setTimeout(30_000, () =>
          req.destroy(new Error(`no response to ${path} in 30 seconds`))
        );
        req.on('error', reject);
        req.end(body);
      })
  );
}

// The parsed JSON body of a 200 response to `options`, as for `request`.
export async function json(server, options) {
  const { status, body, front } = await request(server, options);
  assert.strictEqual(status, 200, `${options.path}: ${body}`);
  return { front, value: JSON.parse(body.toString('utf8')) };
}

// The isolates of the server, one per thread, by identity: each round sends
// a request per thread at once, each answered after a while, and a thread
// with one in flight is passed over for one with none.
export async function threadsOf(server) {
  const fronts = new Set();
  for (let round = 0; round < ROUNDS && fronts.size < server.threads; round++) {
    const answers = [];
    for (let i = 0; i < server.threads; i++) {
      answers.push(request(server, { path: '/front?ms=250' }));
      await scheduler.wait(2);
    }
    for (const { front } of await Promise.all(answers)) fronts.add(front);
  }
  assert.strictEqual(fronts.size, server.threads, 'isolates that served');
  return [...fronts];
}

// Keeps the thread of the isolate `thread` in JavaScript for `ms`
// milliseconds once its head has arrived: resolves with `{spun}`, a promise
// of the spin's `{start, end}`, as soon as the spin is about to start.
export async function spin(server, thread, ms) {
  return steer(
    server,
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request({
          agent: false,
          host: '127.0.0.1',
          port: server.port,
          path: `/spin-body?ms=${ms}`,
          headers,
        });
        req.on('error', reject);
        req.on('response', (res) => {
          const chunks = [];
          res.on('data', (chunk) => chunks.push(chunk));
          const ended = new Promise((done, failed) => {
            res.on('error', failed);
            res.on('end', () => done(Buffer.concat(chunks).toString('utf8')));
          });
          if (res.statusCode === 409) {
            ended.then(() => resolve(undefined), reject);
            return;
          }
          assert.strictEqual(res.statusCode, 200);
          resolve({ spun: ended.then((text) => JSON.parse(text)) });
        });
        req.end();
      })
  );
}
