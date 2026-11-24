// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `bucket` service and a `cache` service under `workerd serve --threads`: each exists once in
// the process, on the server's first thread, so what a Worker writes on one thread the Worker
// reads on every other, and the same checks pass on one thread.
//
// Whichever thread takes a request serves it, whatever connection it came on, so the tests learn
// the threads by the identities of their isolates and have the thread they choose serve what they
// test: a request names the isolate it is for (`x-thread`), any other isolate refuses it before
// doing anything with it (409), and the test sends it again until the one it names serves it
// (`steer`). Which thread is the first is not known to the tests: each check writes on every
// thread in turn and reads on every thread, so that it reads on the first thread what a replica
// wrote, and on replicas what the first thread and other replicas wrote.
import { spawn } from 'node:child_process';
import { createHash, randomBytes, randomUUID } from 'node:crypto';
import { mkdtempSync } from 'node:fs';
import http from 'node:http';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WORKERD_SERVICES_CONFIG !== undefined,
  'You must set the WORKERD_SERVICES_CONFIG environment variable.'
);

const THREADS = 4;
const UUID = /^[0-9a-f-]{36}$/;

// How the server ends on SIGTERM, as `[code, signal]`: Node's SIGTERM is TerminateProcess on
// Windows.
const SIGTERM_EXIT =
  process.platform === 'win32' ? [null, 'SIGTERM'] : [0, null];

// A new directory for the server's `name` service.
function directory(name) {
  return mkdtempSync(path.join(env.TEST_TMPDIR ?? tmpdir(), `${name}-`));
}

// Starts `workerd serve` on `threads` threads with new directories for the bucket's disk and the
// cache. `log` is what the server has written so far.
async function serve(threads) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      env.WORKERD_SERVICES_CONFIG,
      '--experimental',
      `--threads=${threads}`,
      '--control-fd=3',
      `--directory-path=storage=${directory('storage')}`,
      `--directory-path=cache=${directory('cache')}`,
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe'] }
  );
  const server = { child, threads, log: '', port: undefined };
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

  const control = child.stdio[3];
  control.setEncoding('utf8');
  let events = '';
  for await (const chunk of control) {
    events += chunk;
    if (events.includes('\n')) break;
  }
  const event = JSON.parse(events.split('\n')[0]);
  assert.strictEqual(event.event, 'listen', server.log);
  server.port = event.port;
  return server;
}

// Runs `body` with a server on `threads` threads, which then stops on SIGTERM.
async function withServer(threads, body) {
  const server = await serve(threads);
  try {
    await body(server);
  } catch (error) {
    server.child.kill('SIGKILL');
    await server.closed();
    throw error;
  }
  assert.deepStrictEqual(await server.stop(), SIGTERM_EXIT);
}

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

// Sends a request until the isolate of `front` whose identity is `thread` serves it, or once to
// any isolate if `thread` is undefined, on a server of `threads` threads. `attempt(headers)`
// sends the request once with `headers` and resolves with what it got, or with undefined if
// another isolate refused it.
//
// The front wakes the sleeping thread with the fewest requests in flight for each request, and
// among equals the one that slept last, so one request at a time would keep going to the same warm
// thread. Steering fans out instead: a round sends several attempts a few milliseconds apart, and
// an isolate that is not the one named holds its refusal for a while, so that it has a request in
// flight and the next attempt goes to another thread. Each round sends twice as many, so that a
// thread with requests of its own in flight is reached too. The attempts share an `x-attempt`, so
// the isolate named acts on one of them only.
async function steer(thread, attempt, threads = THREADS) {
  if (thread === undefined) {
    const answer = await attempt({});
    assert.notStrictEqual(answer, undefined, 'a request was refused');
    return answer;
  }
  const headers = { 'x-thread': thread, 'x-attempt': randomUUID() };
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
    if (answers.length > 0) return answers[0];
  }
  assert.fail(`isolate ${thread} served none of ${ROUNDS} rounds of requests`);
}

// The response to a `method` request for `path` with `body` (none if undefined), served by the
// isolate `thread` (any if undefined): `{ status, headers, body }`, the body a Buffer.
function call(server, thread, method, path, body) {
  return steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(
          {
            agent: false,
            host: '127.0.0.1',
            port: server.port,
            path,
            method,
            headers,
          },
          (res) => {
            const chunks = [];
            res.on('data', (chunk) => chunks.push(chunk));
            res.on('error', reject);
            res.on('end', () => {
              if (
                res.statusCode === 409 &&
                res.headers['x-refused'] !== undefined
              ) {
                resolve(undefined);
                return;
              }
              const answer = {
                status: res.statusCode,
                headers: res.headers,
                body: Buffer.concat(chunks),
              };
              try {
                assert.match(
                  res.headers['x-front'] ?? '',
                  UUID,
                  `${method} ${path}: ${res.statusCode} ${answer.body}`
                );
                if (thread !== undefined) {
                  assert.strictEqual(res.headers['x-front'], thread);
                }
                resolve(answer);
              } catch (error) {
                reject(error);
              }
            });
          }
        );
        req.setTimeout(20_000, () => {
          req.destroy(new Error(`no response to ${path} in 20 seconds`));
        });
        req.on('error', reject);
        req.end(body);
      }),
    server.threads
  );
}

// The parsed JSON response to a POST of `path` with `body`, served by the isolate `thread`.
async function json(server, thread, path, body) {
  const answer = await call(server, thread, 'POST', path, body);
  assert.strictEqual(answer.status, 200, answer.body.toString());
  return JSON.parse(answer.body.toString());
}

// The isolates of `front`, one per thread, by identity. A round sends a request per thread at
// once, each answered after a while: a thread with one in flight is passed over for one with
// none, so a round usually finds every thread.
async function threadsOf(server) {
  const fronts = new Set();
  for (let round = 0; round < ROUNDS && fronts.size < server.threads; round++) {
    const answers = [];
    for (let i = 0; i < server.threads; i++) {
      answers.push(json(server, undefined, '/where?ms=250'));
      await scheduler.wait(2);
    }
    for (const { front } of await Promise.all(answers)) fronts.add(front);
  }
  assert.strictEqual(
    fronts.size,
    server.threads,
    'isolates that served requests'
  );
  return [...fronts];
}

// The quoted MD5 of `bytes` in hex: the `ETag` the bucket gives an object written whole.
function etagOf(bytes) {
  return `"${createHash('md5').update(bytes).digest('hex')}"`;
}

// Writes `bytes` as the object `key` through the isolate `thread`; the object's `ETag` and
// `x-version`.
async function putObject(server, thread, key, bytes) {
  const put = await call(server, thread, 'PUT', `/bucket/${key}`, bytes);
  assert.strictEqual(put.status, 200, put.body.toString());
  assert.strictEqual(put.headers.etag, etagOf(bytes));
  assert.notStrictEqual(put.headers['x-version'], undefined);
  return { etag: put.headers.etag, version: put.headers['x-version'] };
}

// Checks that the isolate `thread` reads the object `key` as `bytes`, written as `written`.
async function readsObject(server, thread, key, bytes, written) {
  const get = await call(server, thread, 'GET', `/bucket/${key}`);
  assert.strictEqual(get.status, 200, `GET ${key} on ${thread}`);
  assert.strictEqual(get.headers.etag, written.etag);
  assert.strictEqual(get.headers['x-version'], written.version);
  assert.strictEqual(get.body.length, bytes.length);
  assert(get.body.equals(bytes), `the bytes of ${key} read on ${thread}`);
  const head = await call(server, thread, 'HEAD', `/bucket/${key}`);
  assert.strictEqual(head.status, 200, `HEAD ${key} on ${thread}`);
  assert.strictEqual(head.headers.etag, written.etag);
  assert.strictEqual(head.headers['content-length'], String(bytes.length));
}

// Checks that the isolate `thread` finds no object `key`.
async function missesObject(server, thread, key) {
  const get = await call(server, thread, 'GET', `/bucket/${key}`);
  assert.strictEqual(get.status, 404, `GET ${key} on ${thread}`);
  assert.strictEqual(JSON.parse(get.body.toString()).code, 'NoSuchKey');
  const head = await call(server, thread, 'HEAD', `/bucket/${key}`);
  assert.strictEqual(head.status, 404, `HEAD ${key} on ${thread}`);
}

// The query that names the entry `key` of the cache `name` (`caches.default` if undefined).
function entry(key, name) {
  const query = new URLSearchParams({ key });
  if (name !== undefined) query.set('name', name);
  return query.toString();
}

// Checks that the isolate `thread` matches the entry `key` of the cache `name` as `text`, or
// misses it if `text` is undefined.
async function matches(server, thread, key, name, text) {
  const match = await json(server, thread, `/cache/match?${entry(key, name)}`);
  const what = `${name ?? 'default'} ${key} on ${thread}`;
  if (text === undefined) {
    assert.deepStrictEqual(match, { front: thread, hit: false }, what);
  } else {
    assert.deepStrictEqual(
      match,
      { front: thread, hit: true, status: 200, cacheStatus: 'HIT', body: text },
      what
    );
  }
}

for (const threads of [THREADS, 1]) {
  const on = `on ${threads} thread${threads > 1 ? 's' : ''}`;

  test(`an object written on one thread reads the same on every thread ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      for (const [i, writer] of all.entries()) {
        const key = `objects/${i}/${randomUUID()}`;
        const bytes = randomBytes(1000 + i);
        const written = await putObject(server, writer, key, bytes);
        await Promise.all(
          all.map((reader) => readsObject(server, reader, key, bytes, written))
        );
      }
    }));

  test(`an object deleted on one thread is gone on every thread ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      for (const [i, writer] of all.entries()) {
        const deleter = all[(i + 1) % all.length];
        const key = `deleted-${i}`;
        const bytes = randomBytes(100);
        const written = await putObject(server, writer, key, bytes);
        await readsObject(server, deleter, key, bytes, written);
        const deleted = await call(server, deleter, 'DELETE', `/bucket/${key}`);
        assert.strictEqual(deleted.status, 204, deleted.body.toString());
        await Promise.all(
          all.map((reader) => missesObject(server, reader, key))
        );
      }
    }));

  test(`a large object round-trips through every thread ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      for (const [i, writer] of all.entries()) {
        const reader = all[(i + 1) % all.length];
        const key = `large-${i}`;
        // More than 1 MiB, and less than the 8 MiB above which a write is a multipart upload,
        // so that the object's `ETag` is the MD5 of all of it.
        const bytes = randomBytes(3 * 1024 * 1024 + 17);
        const written = await putObject(server, writer, key, bytes);
        await readsObject(server, reader, key, bytes, written);
      }
    }));

  test(`an entry put on one thread matches on every thread ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      for (const [i, writer] of all.entries()) {
        const key = `put-${i}`;
        const text = `cached on ${writer}`;
        await json(server, writer, `/cache/put?${entry(key)}`, text);
        await Promise.all(
          all.map((reader) => matches(server, reader, key, undefined, text))
        );
      }
    }));

  test(`an entry deleted on one thread misses on every thread ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      for (const [i, writer] of all.entries()) {
        const deleter = all[(i + 1) % all.length];
        const key = `deleted-${i}`;
        await json(server, writer, `/cache/put?${entry(key)}`, 'doomed');
        await matches(server, deleter, key, undefined, 'doomed');
        const { deleted } = await json(
          server,
          deleter,
          `/cache/delete?${entry(key)}`
        );
        assert.strictEqual(deleted, true);
        await Promise.all(
          all.map((reader) => matches(server, reader, key, undefined))
        );
        const again = await json(server, writer, `/cache/delete?${entry(key)}`);
        assert.strictEqual(again.deleted, false);
      }
    }));

  test(`a named cache is apart from the default one on every thread ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      const everywhere = (key, name, text) =>
        Promise.all(all.map((t) => matches(server, t, key, name, text)));
      for (const [i, writer] of all.entries()) {
        const other = all[(i + 1) % all.length];
        const key = `shared-${i}`;
        await json(
          server,
          writer,
          `/cache/put?${entry(key, 'named')}`,
          'named'
        );
        await everywhere(key, 'named', 'named');
        await everywhere(key, undefined, undefined);
        await everywhere(key, 'another', undefined);
        await json(server, other, `/cache/put?${entry(key)}`, 'default');
        await everywhere(key, undefined, 'default');
        await everywhere(key, 'named', 'named');
        const { deleted } = await json(
          server,
          writer,
          `/cache/delete?${entry(key, 'named')}`
        );
        assert.strictEqual(deleted, true);
        await everywhere(key, 'named', undefined);
        await everywhere(key, undefined, 'default');
      }
    }));
}
