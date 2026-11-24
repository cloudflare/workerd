// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Durable Objects under `workerd serve --threads`: every object runs on the server's first
// thread, and a request another thread serves reaches it over that thread's session.
//
// Whichever thread takes a request serves it, whatever connection it came on, so the tests learn
// the threads by the identities of their isolates and have the thread they choose serve what they
// test: a request names the isolate it is for (`x-thread`), any other isolate refuses it before
// doing anything with it (409), and the test sends it again until the one it names serves it
// (`steer`).
import { spawn } from 'node:child_process';
import { randomBytes, randomUUID } from 'node:crypto';
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
  env.WORKERD_DURABLE_CONFIG !== undefined,
  'You must set the WORKERD_DURABLE_CONFIG environment variable.'
);

const THREADS = 4;
const UUID = /^[0-9a-f-]{36}$/;

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

// Starts `workerd serve` on `threads` threads with a new directory for the objects. `log` is
// what the server has written so far.
async function serve(threads) {
  const directory = mkdtempSync(
    path.join(env.TEST_TMPDIR ?? tmpdir(), 'objects-')
  );
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

// How many times a test asks before it gives up on what it waits for.
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

// The options of a request for `path` on a connection of its own.
function options(server, path, more = {}) {
  return { agent: false, host: '127.0.0.1', port: server.port, path, ...more };
}

// The parsed JSON response to a GET of `path`, served by the isolate `thread` (any if
// undefined).
function get(server, path, thread) {
  return steer(
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
              assert.strictEqual(res.statusCode, 200, body);
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
      }),
    server.threads
  );
}

// A request for `path` whose response head the isolate `thread` sent, with its body still to
// write: `{ req, res }`.
function open(server, path, thread, more) {
  return steer(
    thread,
    (headers) =>
      new Promise((resolve, reject) => {
        const req = http.request(options(server, path, { ...more, headers }));
        req.on('error', reject);
        req.on('response', (res) => {
          if (res.statusCode === 409) {
            res.resume();
            req.destroy();
            resolve(undefined);
          } else {
            resolve({ req, res });
          }
        });
        req.flushHeaders();
      }),
    server.threads
  );
}

// The isolates of `front`, one per thread, by identity: `first` is the first thread's, whose
// copy of the Worker runs the objects, so it is also `object`, the identity the objects report;
// `replicas` are the others'. A round sends a request per thread at once, each answered after a
// while: a thread with one in flight is passed over for one with none, so a round usually finds
// every thread.
async function threadsOf(server, threads = THREADS) {
  const fronts = new Set();
  let object;
  for (let round = 0; round < ROUNDS && fronts.size < threads; round++) {
    const answers = [];
    for (let i = 0; i < threads; i++) {
      answers.push(get(server, '/where?name=probe&ms=250'));
      await scheduler.wait(2);
    }
    for (const where of await Promise.all(answers)) {
      assert.match(where.front, UUID);
      assert.strictEqual(object ?? where.isolate, where.isolate);
      object = where.isolate;
      fronts.add(where.front);
    }
  }
  assert.strictEqual(fronts.size, threads, 'isolates that served requests');
  assert(fronts.has(object), 'the first thread served no request');
  const replicas = [...fronts].filter((front) => front !== object);
  return { object, first: object, replicas, all: [object, ...replicas] };
}

// The isolate the tests send to: one of a thread without the objects, if there is one.
function away({ first, replicas }) {
  return replicas[0] ?? first;
}

// A WebSocket opened with a request for `path` that the isolate `thread` served: just enough
// of the protocol for short text messages.
function webSocket(server, path, thread) {
  return steer(
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
      }),
    server.threads
  );
}

for (const threads of [THREADS, 1]) {
  const on = `on ${threads} thread${threads > 1 ? 's' : ''}`;
  const supported = threads > 1 ? SEVERAL_THREADS : {};

  test(`requests of every thread count in one object ${on}`, supported, () =>
    withServer(threads, async (server) => {
      const { all, object } = await threadsOf(server, threads);
      const PER_THREAD = 40;
      const counts = await Promise.all(
        all.flatMap((front) =>
          Array.from({ length: PER_THREAD }, async () => {
            const answer = await get(server, '/count?name=shared', front);
            assert.strictEqual(answer.front, front);
            // One object, in the isolate of the thread that has the objects.
            assert.strictEqual(answer.isolate, object);
            return answer.count;
          })
        )
      );
      const total = all.length * PER_THREAD;
      assert.deepStrictEqual(
        counts.sort((a, b) => a - b),
        Array.from({ length: total }, (_, i) => i + 1)
      );
    })
  );

  test(`a stub does what stubs do ${on}`, supported, () =>
    withServer(threads, async (server) => {
      const { all, object } = await threadsOf(server, threads);
      const results = await Promise.all(
        all.map((front, i) =>
          get(server, `/features?name=features-${i}`, front)
        )
      );
      for (const [i, result] of results.entries()) {
        const front = all[i];
        const at = { isolate: object };
        assert.match(result.service.front, UUID);
        assert.deepStrictEqual(result, {
          front,
          // The name the id was made from reaches the object.
          name: `features-${i}`,
          // A stub the object returned, called in order.
          session: { sums: [1, 3], ...at },
          // Calls on one stub arrive in the order they were made.
          recorded: Array.from({ length: 50 }, (_, n) => n),
          // A stub passed to the object names the same object there.
          other: { count: 1, ...at },
          otherAgain: { count: 2, ...at },
          service: result.service,
          // An entrypoint with props, passed to the object, runs on the object's thread.
          greeter: { greeting: 'hello, object', ...at },
          // A function passed to the object runs where it was made.
          callBack: { object, front },
          facet: [
            { bumps: 1, ...at },
            { bumps: 2, ...at },
            { bumps: 1, ...at },
          ],
          error: { name: 'RangeError', message: 'the object refused' },
          ephemeral: { count: 2, ...at },
          exported: { count: 2, ...at },
          connect: `object ${object}`,
          ephemeralArgument:
            'Stubs pointing to ephemeral objects are not serializable.',
          // A stub the object vended through `restore()`.
          vended: 4,
          // Restored from the thread that got it, by a new run of the object's `[restore]()`.
          restored: 4,
          // A service restored the same way, from parameters that hold a service: it runs on
          // the object's thread.
          restoredGreeter: { greeting: 'restored, front', ...at },
          // A stub of a facet that the object's `[restore]()` gives, restored through both.
          restoredChain: 6,
        });
        // A service binding passed to the object is the binding of the object's thread.
        assert.notStrictEqual(result.service.front, front);
        assert.strictEqual(result.service.front, results[0].service.front);
      }
    })
  );

  test(`bodies stream to an object and back ${on}`, supported, () =>
    withServer(threads, async (server) => {
      const fronts = await threadsOf(server, threads);
      const front = away(fronts);
      // The response head comes before any of the body was sent.
      const { req, res } = await open(server, '/echo', front, {
        method: 'POST',
      });
      assert.strictEqual(res.statusCode, 200);
      assert.strictEqual(res.headers['x-front'], front);
      assert.strictEqual(res.headers['x-object'], fronts.object);
      const received = [];
      let waiter;
      const done = new Promise((resolve, reject) => {
        req.on('error', reject);
        res.on('data', (chunk) => {
          received.push(chunk);
          waiter?.();
        });
        res.on('error', reject);
        res.on('end', resolve);
      });
      const size = () => received.reduce((n, chunk) => n + chunk.length, 0);
      const receivedAtLeast = async (bytes) => {
        while (size() < bytes) {
          await new Promise((resolve) => (waiter = resolve));
        }
      };

      // The response to the first of the body arrives while the rest is still to come.
      const sent = [Buffer.from('abcdefghij'.repeat(100))];
      req.write(sent[0]);
      await receivedAtLeast(sent[0].length);
      // Then more than anything on the way buffers.
      for (let i = 0; i < 64; i++) {
        const chunk = Buffer.alloc(64 * 1024, 97 + (i % 26));
        sent.push(chunk);
        if (!req.write(chunk)) {
          await new Promise((resolve) => req.once('drain', resolve));
        }
      }
      req.end();
      await done;
      const expected = Buffer.concat(sent).toString('latin1').toUpperCase();
      assert.strictEqual(size(), expected.length);
      assert(Buffer.concat(received).toString('latin1') === expected);
    })
  );

  test(`an alarm set through a stub fires once ${on}`, supported, () =>
    withServer(threads, async (server) => {
      const fronts = await threadsOf(server, threads);
      const front = away(fronts);
      await get(server, '/alarm?name=alarm&ms=300', front);
      await scheduler.wait(1500);
      const { alarms } = await get(server, '/alarms?name=alarm', front);
      assert.strictEqual(alarms, 1);
      await scheduler.wait(1000);
      const later = await get(server, '/alarms?name=alarm', fronts.first);
      assert.strictEqual(later.alarms, 1);
    })
  );

  test(
    `the hooks that end every object work on the first thread only ${on}`,
    supported,
    () =>
      withServer(threads, async (server) => {
        const { first, replicas, object } = await threadsOf(server, threads);
        for (const hook of ['abort', 'delete']) {
          const before = await get(server, `/where?name=${hook}`, first);
          for (const front of replicas) {
            const { error } = await get(server, `/${hook}-all`, front);
            assert.strictEqual(
              error,
              `Cannot ${hook} Durable Objects from this thread: with --threads, Durable ` +
                "Objects run on the server's first thread, and only a request that thread " +
                'serves can do this.'
            );
            // The object was left as it was.
            const still = await get(server, `/where?name=${hook}`, front);
            assert.strictEqual(still.instance, before.instance);
          }
          const all = await get(server, `/${hook}-all`, first);
          assert.strictEqual(all.error, null);
          const after = await get(server, `/where?name=${hook}`, first);
          assert.strictEqual(after.isolate, object);
          assert.notStrictEqual(after.instance, before.instance);
        }
      })
  );

  test(
    `SIGTERM completes the object calls in flight ${on}`,
    SIGTERM_DRAINS,
    async () => {
      const server = await serve(threads);
      try {
        const { all } = await threadsOf(server, threads);
        // Each thread has a request in the object when the server is told to stop, and the
        // object answers after that.
        const slow = all.map((front) =>
          get(server, '/slow?name=drain&ms=1500', front)
        );
        for (let tries = 0; ; tries++) {
          const { calls } = await get(server, '/slow-calls?name=drain');
          if (calls === all.length) break;
          assert(tries < TRIES, `${calls} of ${all.length} calls arrived`);
          await scheduler.wait(10);
        }
        server.child.kill('SIGTERM');
        const answers = await Promise.all(slow);
        assert.deepStrictEqual(
          answers.map((a) => a.front),
          all
        );
        assert.deepStrictEqual(
          answers.map((a) => a.count).sort((a, b) => a - b),
          answers.map((_, i) => i + 1)
        );
        assert.deepStrictEqual(await server.exited(), [0, null]);
      } catch (error) {
        server.child.kill('SIGKILL');
        await server.closed();
        throw error;
      }
    }
  );
}

test(
  'an idle object is shut down and a hibernating WebSocket wakes it',
  SEVERAL_THREADS,
  () =>
    withServer(THREADS, async (server) => {
      const { object, replicas } = await threadsOf(server);
      assert(replicas.length >= 2, 'two threads without the objects');
      const [plainFront, hibernatingFront] = replicas;
      const front = replicas.at(-1);

      // A WebSocket the object holds itself.
      const plain = await webSocket(server, '/ws?name=plain', plainFront);
      assert.strictEqual(plain.front, plainFront);
      plain.send('one');
      const one = await plain.next();
      assert.deepStrictEqual(
        { echo: one.echo, messages: one.messages, isolate: one.isolate },
        { echo: 'one', messages: 1, isolate: object }
      );
      plain.send('two');
      assert.deepStrictEqual(await plain.next(), {
        ...one,
        echo: 'two',
        messages: 2,
      });

      // A WebSocket the object hibernates with, which a thread without the object took.
      const hibernating = await webSocket(
        server,
        '/ws?name=hibernating&hibernate',
        hibernatingFront
      );
      assert.strictEqual(hibernating.front, hibernatingFront);
      hibernating.send('before');
      const before = await hibernating.next();
      assert.strictEqual(before.echo, 'before');
      assert.strictEqual(before.isolate, object);

      // An object nothing holds: the stub its request used is gone with the request.
      const idle = await get(server, '/where?name=idle', front);
      assert.strictEqual(idle.isolate, object);

      // Past the ten seconds after which an idle object is shut down.
      await scheduler.wait(11500);

      // Each was constructed again: nothing on another thread kept the first instance alive.
      const again = await get(server, '/where?name=idle', front);
      assert.notStrictEqual(again.instance, idle.instance);
      assert.strictEqual(again.isolate, object);

      assert(!hibernating.isClosed());
      hibernating.send('after');
      const after = await hibernating.next();
      assert.deepStrictEqual(
        { echo: after.echo, messages: after.messages, isolate: after.isolate },
        { echo: 'after', messages: 1, isolate: object }
      );
      assert.notStrictEqual(after.instance, before.instance);

      // The object with a WebSocket of its own could not hibernate, and was not shut down.
      plain.send('three');
      assert.deepStrictEqual(await plain.next(), {
        ...one,
        echo: 'three',
        messages: 3,
      });

      await plain.close();
      await hibernating.close();
      assert.doesNotMatch(server.log, /internal bug in hibernation/);
    })
);
