// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Durable Objects under `workerd serve --threads`: every object runs on the server's first
// thread, and a request another thread serves reaches it over that thread's session.
//
// A connection stays with the thread that accepted it, so the tests keep connections open,
// learn which thread each belongs to, and send what they test over one of a thread that has no
// objects.
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
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

// How the tests serve on several threads: `threads`, a loop of its own on every thread, or
// `units`, as many loops as threads on a work-stealing scheduler, each with its own copy of every
// Worker, which serves the same as `threads` does.
const MODE = env.WORKERD_THREADS_MODE ?? 'threads';
assert(
  MODE === 'threads' || MODE === 'units',
  `WORKERD_THREADS_MODE must be threads or units, not ${MODE}`
);
// One thread serves the same in either mode: `threads` mode tests it.
const ONE_THREAD = { skip: MODE !== 'threads' && 'threads mode tests this' };

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
      ...(threads > 1 ? threadArgs(threads) : []),
      '--control-fd=3',
      `--directory-path=objects=${directory}`,
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe'] }
  );
  const server = { child, log: '', port: undefined };
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
  server.stop = async () => {
    child.kill('SIGTERM');
    return await closed;
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

// One connection to the server, kept open: every request on it is served by the thread that
// accepted it.
class Connection {
  constructor(port) {
    this.port = port;
    this.agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
    // The identity of the isolate of `front` that serves this connection.
    this.front = undefined;
  }

  options(path, more = {}) {
    return {
      agent: this.agent,
      host: '127.0.0.1',
      port: this.port,
      path,
      ...more,
    };
  }

  // The parsed JSON response to a GET of `path`.
  get(path) {
    return new Promise((resolve, reject) => {
      const req = http.request(this.options(path), (res) => {
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
      req.on('error', reject);
      req.end();
    });
  }

  close() {
    this.agent.destroy();
  }
}

// Connections to the server by the thread that serves them: `first` to the first thread, which
// has the objects, `replicas` to the others, by isolate. `object` is the identity of the isolate
// the objects run in.
async function connect(server, threads = THREADS) {
  const { port } = server;
  const all = [];
  const byFront = new Map();
  let object;
  // Which thread accepts a connection is the kernel's choice, so open them while a thread is
  // busy until enough threads have one.
  for (let attempt = 0; attempt < 40; attempt++) {
    const busy = new Connection(port);
    const spin = busy.get('/spin?ms=200');
    const opened = Array.from({ length: 8 }, () => new Connection(port));
    all.push(busy, ...opened);
    await scheduler.wait(20);
    for (const [connection, where] of await Promise.all(
      opened.map(async (c) => [c, await c.get('/where?name=probe')])
    )) {
      assert.match(where.front, UUID);
      assert.strictEqual(object ?? where.isolate, where.isolate);
      object = where.isolate;
      connection.front = where.front;
      if (!byFront.has(where.front)) byFront.set(where.front, connection);
    }
    busy.front = (await spin).front;
    if (!byFront.has(busy.front)) byFront.set(busy.front, busy);
    if (byFront.has(object) && byFront.size >= Math.min(threads, 3)) break;
  }
  assert(byFront.size <= threads, `${byFront.size} isolates of one Worker`);
  const first = byFront.get(object);
  assert(first !== undefined, 'the first thread served no connection');
  const replicas = [...byFront.values()].filter((c) => c !== first);
  assert.strictEqual(replicas.length > 0, threads > 1, 'threads that served');
  for (const connection of all) {
    if (
      !byFront.has(connection.front) ||
      byFront.get(connection.front) !== connection
    ) {
      connection.close();
    }
  }
  return {
    object,
    first,
    replicas,
    all: [first, ...replicas],
    close: () => [first, ...replicas].forEach((c) => c.close()),
  };
}

// The connection the tests send over: one of a thread without the objects, if there is one.
function away({ first, replicas }) {
  return replicas[0] ?? first;
}

// A WebSocket over `connection`, opened with a request for `path`: just enough of the protocol
// for short text messages.
function webSocket(connection, path) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      connection.options(path, {
        headers: {
          Connection: 'Upgrade',
          Upgrade: 'websocket',
          'Sec-WebSocket-Version': '13',
          'Sec-WebSocket-Key': randomBytes(16).toString('base64'),
        },
      })
    );
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
              waiting.push((m) => resolve(m === null ? null : JSON.parse(m)));
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
  });
}

for (const threads of [THREADS, 1]) {
  const on = `on ${threads} thread${threads > 1 ? 's' : ''}`;
  const supported = threads > 1 ? SEVERAL_THREADS : ONE_THREAD;

  test(`requests of every thread count in one object ${on}`, supported, () =>
    withServer(threads, async (server) => {
      const connections = await connect(server, threads);
      const { all, object } = connections;
      const PER_CONNECTION = 40;
      const counts = await Promise.all(
        all.flatMap((connection) =>
          Array.from({ length: PER_CONNECTION }, async () => {
            const answer = await connection.get('/count?name=shared');
            assert.strictEqual(answer.front, connection.front);
            // One object, in the isolate of the thread that has the objects.
            assert.strictEqual(answer.isolate, object);
            return answer.count;
          })
        )
      );
      const total = all.length * PER_CONNECTION;
      assert.deepStrictEqual(
        counts.sort((a, b) => a - b),
        Array.from({ length: total }, (_, i) => i + 1)
      );
      connections.close();
    })
  );

  test(`a stub does what stubs do ${on}`, supported, () =>
    withServer(threads, async (server) => {
      const connections = await connect(server, threads);
      const { object } = connections;
      const results = await Promise.all(
        connections.all.map((c, i) => c.get(`/features?name=features-${i}`))
      );
      for (const [i, result] of results.entries()) {
        const connection = connections.all[i];
        const at = { isolate: object };
        assert.match(result.service.front, UUID);
        assert.deepStrictEqual(result, {
          front: connection.front,
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
          callBack: { object, front: connection.front },
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
        assert.notStrictEqual(result.service.front, connection.front);
        assert.strictEqual(result.service.front, results[0].service.front);
      }
      connections.close();
    })
  );

  test(`bodies stream to an object and back ${on}`, supported, () =>
    withServer(threads, async (server) => {
      const connections = await connect(server, threads);
      const connection = away(connections);
      const received = [];
      let waiter;
      const req = http.request(connection.options('/echo', { method: 'POST' }));
      const done = new Promise((resolve, reject) => {
        req.on('error', reject);
        req.on('response', (res) => {
          assert.strictEqual(res.statusCode, 200);
          assert.strictEqual(res.headers['x-front'], connection.front);
          assert.strictEqual(res.headers['x-object'], connections.object);
          res.on('data', (chunk) => {
            received.push(chunk);
            waiter?.();
          });
          res.on('error', reject);
          res.on('end', resolve);
        });
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
      connections.close();
    })
  );

  test(`an alarm set through a stub fires once ${on}`, supported, () =>
    withServer(threads, async (server) => {
      const connections = await connect(server, threads);
      const connection = away(connections);
      await connection.get('/alarm?name=alarm&ms=300');
      await scheduler.wait(1500);
      const { alarms } = await connection.get('/alarms?name=alarm');
      assert.strictEqual(alarms, 1);
      await scheduler.wait(1000);
      const later = await connections.first.get('/alarms?name=alarm');
      assert.strictEqual(later.alarms, 1);
      connections.close();
    })
  );

  test(
    `the hooks that end every object work on the first thread only ${on}`,
    supported,
    () =>
      withServer(threads, async (server) => {
        const connections = await connect(server, threads);
        const { first, object } = connections;
        for (const hook of ['abort', 'delete']) {
          const before = await first.get(`/where?name=${hook}`);
          for (const connection of connections.replicas) {
            const { error } = await connection.get(`/${hook}-all`);
            assert.strictEqual(
              error,
              `Cannot ${hook} Durable Objects from this thread: with --threads, Durable ` +
                "Objects run on the server's first thread, and only a request that thread " +
                'serves can do this.'
            );
            // The object was left as it was.
            const still = await connection.get(`/where?name=${hook}`);
            assert.strictEqual(still.instance, before.instance);
          }
          assert.strictEqual((await first.get(`/${hook}-all`)).error, null);
          const after = await first.get(`/where?name=${hook}`);
          assert.strictEqual(after.isolate, object);
          assert.notStrictEqual(after.instance, before.instance);
        }
        connections.close();
      })
  );

  test(
    `SIGTERM completes the object calls in flight ${on}`,
    { skip: SIGTERM_DRAINS.skip || (threads === 1 && ONE_THREAD.skip) },
    async () => {
      const server = await serve(threads);
      const connections = await connect(server, threads);
      // Each connection has a request in its object when the server is told to stop, and the
      // object answers after that.
      const slow = connections.all.map((c) =>
        c.get('/slow?name=drain&ms=1500')
      );
      await scheduler.wait(500);
      server.child.kill('SIGTERM');
      const answers = await Promise.all(slow);
      assert.deepStrictEqual(
        answers.map((a) => a.front),
        connections.all.map((c) => c.front)
      );
      assert.deepStrictEqual(
        answers.map((a) => a.count).sort((a, b) => a - b),
        answers.map((_, i) => i + 1)
      );
      connections.close();
      assert.deepStrictEqual(await server.closed(), [0, null]);
    }
  );
}

test(
  'an idle object is shut down and a hibernating WebSocket wakes it',
  SEVERAL_THREADS,
  () =>
    withServer(THREADS, async (server) => {
      const connections = await connect(server);
      const { object } = connections;
      assert(
        connections.replicas.length >= 2,
        'two threads without the objects'
      );
      const [plainConnection, hibernatingConnection] = connections.replicas;
      const connection = connections.replicas.at(-1);

      // A WebSocket the object holds itself.
      const plain = await webSocket(plainConnection, '/ws?name=plain');
      assert.strictEqual(plain.front, plainConnection.front);
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

      // A WebSocket the object hibernates with, on a connection of a thread without the object.
      const hibernating = await webSocket(
        hibernatingConnection,
        '/ws?name=hibernating&hibernate'
      );
      assert.strictEqual(hibernating.front, hibernatingConnection.front);
      hibernating.send('before');
      const before = await hibernating.next();
      assert.strictEqual(before.echo, 'before');
      assert.strictEqual(before.isolate, object);

      // An object nothing holds: the stub its request used is gone with the request.
      const idle = await connection.get('/where?name=idle');
      assert.strictEqual(idle.isolate, object);

      // Past the ten seconds after which an idle object is shut down.
      await scheduler.wait(11500);

      // Each was constructed again: nothing on another thread kept the first instance alive.
      const again = await connection.get('/where?name=idle');
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
      connections.close();
      assert.doesNotMatch(server.log, /internal bug in hibernation/);
    })
);
