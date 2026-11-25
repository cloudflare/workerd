// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Where the Durable Objects of `workerd serve --threads` live, and what crosses between threads
// to reach them. The process gives every object a home thread, the only one that runs it, and
// any other thread reaches it there over a session between the two threads.
//
// A test cannot tell which thread is the first, nor ask where an object will live: it learns
// the threads by the isolates of their copies of the Worker, and asks objects where they run
// (`homes`) until it has one on each thread. Each case then holds for objects on every thread,
// reached from threads other than their homes.
import { createHash, randomBytes } from 'node:crypto';
import { test } from 'node:test';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';
import {
  SIGTERM_DRAINS,
  assertCleanEnd,
  THREADS,
  TRIES,
  body,
  get,
  homes,
  metadataFiles,
  open,
  others,
  serve,
  steer,
  threadsOf,
  webSocket,
  withServer,
} from './durable-harness.mjs';

const on = `on ${THREADS} threads`;

// Every ordered pair of distinct isolates of `fronts`.
function pairs(fronts) {
  return fronts.flatMap((a) =>
    fronts.filter((b) => b !== a).map((b) => [a, b])
  );
}

function sha256(buffer) {
  return createHash('sha256').update(buffer).digest('hex');
}

function upperCase(buffer) {
  return Buffer.from(
    buffer.map((byte) => (byte >= 97 && byte <= 122 ? byte - 32 : byte))
  );
}

test(`objects live on every thread, each on one ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    const byHome = await homes(server, all, { prefix: 'spread', per: 2 });
    // Each object answers in the same isolate, as the same instance, from every thread.
    await Promise.all(
      [...byHome].flatMap(([home, names]) =>
        names.map(async (name) => {
          const wheres = await Promise.all(
            all.map((front) => get(server, `/where?name=${name}`, front))
          );
          for (const where of wheres) {
            assert.strictEqual(where.isolate, home);
            assert.strictEqual(where.instance, wheres[0].instance);
          }
        })
      )
    );
  }));

test(`an object's counter is exact under requests of every thread ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    const byHome = await homes(server, all, { prefix: 'exact' });
    const PER_THREAD = 50;
    // One object after another: every thread is busy with the one at hand, and a request for a
    // thread that is busy takes long to steer.
    for (const [home, [name]] of byHome) {
      // Each thread makes its increments at once.
      const answers = await Promise.all(
        all.map((front) =>
          get(server, `/increments?name=${name}&n=${PER_THREAD}`, front)
        )
      );
      const results = answers.flatMap((answer) => answer.results);
      for (const { isolate } of results) assert.strictEqual(isolate, home);
      assert.deepStrictEqual(
        results.map((r) => r.count).sort((a, b) => a - b),
        Array.from({ length: all.length * PER_THREAD }, (_, i) => i + 1)
      );
    }
  }));

test(`objects on two threads call each other in a cycle ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    const byHome = await homes(server, all, { prefix: 'cycle' });
    await Promise.all(
      pairs(all).map(async ([homeA, homeB]) => {
        const [a] = byHome.get(homeA);
        const [b] = byHome.get(homeB);
        // Served by a third thread.
        const front = all.find((f) => f !== homeA && f !== homeB);
        const cycle = await get(server, `/cycle?a=${a}&b=${b}`, front);
        const { instance } = cycle;
        assert.deepStrictEqual(cycle, {
          front,
          isolate: homeA,
          instance,
          pong: {
            isolate: homeB,
            instance: cycle.pong.instance,
            // B called A back: the instance that is waiting for B, by JS RPC and by fetch.
            rpc: { isolate: homeA, instance },
            fetched: { isolate: homeA, instance },
          },
        });
      })
    );
  }));

test(`an alarm fires once, at the object's home ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    const byHome = await homes(server, all, { prefix: 'alarm' });
    await Promise.all(
      [...byHome].map(async ([home, [name]]) => {
        const [front, later] = others(all, home);
        await get(server, `/alarm?name=${name}&ms=300`, front);
        let fired;
        for (let tries = 0; tries < TRIES; tries++) {
          fired = await get(server, `/alarms?name=${name}`);
          if (fired.alarms > 0) break;
          await scheduler.wait(20);
        }
        assert.deepStrictEqual(fired.isolates, [home]);
        // Long enough for a second run to show.
        await scheduler.wait(1500);
        const after = await get(server, `/alarms?name=${name}`, later);
        assert.deepStrictEqual(after, {
          front: later,
          alarms: 1,
          isolates: [home],
        });
      })
    );
  }));

test(`a hibernatable WebSocket wakes its object at home after an eviction ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    const byHome = await homes(server, all, { prefix: 'hibernate' });
    const sockets = [];
    try {
      await Promise.all(
        [...byHome].map(async ([home, [name]]) => {
          // Opened through one thread, evicted from another.
          const [front, evicting] = others(all, home);
          const ws = await webSocket(
            server,
            `/ws?name=${name}&hibernate`,
            front
          );
          sockets.push(ws);
          assert.strictEqual(ws.front, front);
          ws.send('one');
          const one = await ws.next();
          assert.deepStrictEqual(one, {
            echo: 'one',
            instance: one.instance,
            messages: 1,
            isolate: home,
          });
          ws.send('two');
          assert.deepStrictEqual(await ws.next(), {
            ...one,
            echo: 'two',
            messages: 2,
          });

          await get(server, `/hibernate?name=${name}`, evicting);
          // Still open; the next message constructs the object again, at its home.
          await scheduler.wait(100);
          assert(!ws.isClosed(), 'the WebSocket closed');
          ws.send('three');
          const three = await ws.next();
          assert.notStrictEqual(three, null, 'the WebSocket closed');
          assert.notStrictEqual(three.instance, one.instance);
          assert.deepStrictEqual(three, {
            echo: 'three',
            instance: three.instance,
            messages: 1,
            isolate: home,
          });
        })
      );
    } finally {
      await Promise.all(sockets.map((ws) => ws.close()));
    }
    assert.doesNotMatch(server.log, /internal bug in hibernation/);
  }));

test(`facets live with their root, made, cloned and deleted from other threads ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    const byHome = await homes(server, all, { prefix: 'facets' });
    await Promise.all(
      [...byHome].map(async ([home, [name]]) => {
        const [one, two, three] = others(all, home);
        const facet = (op, front, rest = '') =>
          get(server, `/facet?name=${name}&op=${op}${rest}`, front);
        const bump = async (front, of, bumps) => {
          assert.deepStrictEqual(await facet('bump', front, `&facet=${of}`), {
            front,
            bumps,
            isolate: home,
          });
        };
        await bump(one, 'f', 1);
        await bump(one, 'f', 2);
        // The clone has a copy of the storage, apart from the original's.
        await facet('clone', two, '&facet=f&to=g');
        await bump(two, 'g', 3);
        await bump(two, 'f', 3);
        await bump(two, 'g', 4);
        // Deleted, it starts again from nothing; the clone keeps its own.
        await facet('delete', three, '&facet=f');
        await bump(three, 'f', 1);
        await bump(three, 'g', 5);
        await bump(one, 'g', 6);
      })
    );
  }));

test(`a stub an object vends is restored on other threads ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    const byHome = await homes(server, all, { prefix: 'restore' });
    await Promise.all(
      [...byHome].flatMap(([home, [name]]) =>
        others(all, home).map(async (front) => {
          // `[restore]()` runs at the object's home, from what the front's thread restored.
          assert.deepStrictEqual(
            await get(server, `/restore?name=${name}`, front),
            {
              front,
              vended: 4,
              vendedAt: home,
              restored: 4,
              restoredAt: home,
            }
          );
        })
      )
    );
  }));

test(`a body streams through one thread to an object on another and back ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    // An object for each pair of a front and a home.
    const byHome = await homes(server, all, {
      prefix: 'stream',
      per: all.length,
    });
    const SIZE = 3 * 1024 * 1024;
    await Promise.all(
      pairs(all).map(async ([front, home], i) => {
        const name = byHome.get(home)[all.indexOf(front)];
        const data = randomBytes(SIZE);
        const { req, res } = await open(
          server,
          `/echo-digest?name=${name}`,
          front,
          { method: 'POST' }
        );
        assert.strictEqual(res.statusCode, 200);
        assert.strictEqual(res.headers['x-front'], front);
        assert.strictEqual(res.headers['x-object'], home);
        const echoed = body(res);
        const CHUNK = 64 * 1024;
        for (let offset = 0; offset < SIZE; offset += CHUNK) {
          if (!req.write(data.subarray(offset, offset + CHUNK))) {
            await new Promise((resolve) => req.once('drain', resolve));
          }
        }
        req.end();
        const back = await echoed;
        assert.strictEqual(back.length, SIZE, `pair ${i}`);
        assert.strictEqual(sha256(back), sha256(upperCase(data)));
        // What the object read is what was sent.
        const received = await get(server, `/received?name=${name}`, front);
        assert.deepStrictEqual(received, {
          front,
          digest: sha256(data),
          bytes: SIZE,
        });
      })
    );
  }));

test(`a WebSocket relays through one thread to an object on another ${on}`, () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    const byHome = await homes(server, all, {
      prefix: 'relay',
      per: all.length,
    });
    const sockets = [];
    try {
      await Promise.all(
        pairs(all).map(async ([front, home]) => {
          const name = byHome.get(home)[all.indexOf(front)];
          const ws = await webSocket(server, `/ws?name=${name}`, front);
          sockets.push(ws);
          assert.strictEqual(ws.front, front);
          let instance;
          for (let n = 1; n <= 5; n++) {
            const text = `${front.slice(0, 8)}-${n}`;
            ws.send(text);
            const reply = await ws.next();
            instance ??= reply?.instance;
            assert.deepStrictEqual(reply, {
              echo: text,
              instance,
              messages: n,
              isolate: home,
            });
          }
        })
      );
    } finally {
      await Promise.all(sockets.map((ws) => ws.close()));
    }
  }));

test(
  `SIGTERM completes the calls in flight to objects on other threads ${on}`,
  SIGTERM_DRAINS,
  async () => {
    const server = await serve(THREADS);
    try {
      const all = await threadsOf(server);
      const byHome = await homes(server, all, { prefix: 'drain' });
      const calls = [...byHome].map(([home, [name]]) => ({
        home,
        name,
        front: others(all, home)[0],
      }));
      const slow = calls.map(({ name, front }) =>
        get(server, `/slow?name=${name}&ms=1000`, front)
      );
      for (const { name } of calls) {
        for (let tries = 0; ; tries++) {
          const answer = await get(server, `/slow-calls?name=${name}`);
          if (answer.calls === 1) break;
          assert(tries < TRIES, `the call of ${name} did not arrive`);
          await scheduler.wait(10);
        }
      }
      // Every other attempt to steer them has been refused.
      await server.quiet(calls.length);
      server.child.kill('SIGTERM');
      const answers = await Promise.all(slow);
      for (const [i, { home, front }] of calls.entries()) {
        assert.deepStrictEqual(answers[i], { front, count: 1, isolate: home });
      }
      const exit = await server.exited();
      assertCleanEnd(server);
      assert.deepStrictEqual(exit, [0, null]);
    } catch (error) {
      server.child.kill('SIGKILL');
      await server.closed();
      throw error;
    }
  }
);

// Has the isolate `spinner` run JavaScript for `ms` milliseconds, and meanwhile the isolate
// `front` call the object `name` `n` times: the spin's `{ start, end }` and the calls.
async function callsDuringSpin(server, { spinner, front, name, n, ms }) {
  const { res } = await open(server, `/spin-body?ms=${ms}`, spinner);
  assert.strictEqual(res.statusCode, 200);
  const spun = body(res);
  // The spin starts once the body is read.
  await scheduler.wait(100);
  const { calls } = await get(server, `/calls?name=${name}&n=${n}`, front);
  const spin = JSON.parse((await spun).toString());
  assert.strictEqual(spin.front, spinner);
  return { spin, calls };
}

test(`a busy thread delays no call between two others ${on}`, async () => {
  // Twice, on a new server each time: the busy threads are three of the four, which may not
  // include the first.
  for (let round = 0; round < 2; round++) {
    await withServer(THREADS, async (server) => {
      const all = await threadsOf(server);
      // Objects found through one thread only: no other pair of threads has called across yet.
      const [via, ...rest] = all;
      const byHome = await homes(server, all, { prefix: 'busy', via });
      for (const spinner of rest) {
        // The first call ever between `front` and `home`, and more, while `spinner` spins.
        const [front, home] = rest.filter((f) => f !== spinner);
        const [name] = byHome.get(home);
        const { spin, calls } = await callsDuringSpin(server, {
          spinner,
          front,
          name,
          n: 5,
          ms: 2500,
        });
        for (const call of calls) {
          assert.strictEqual(call.isolate, home);
          const took = call.end - call.start;
          assert(took < 500, `a call took ${took} ms`);
          assert(
            call.start >= spin.start && call.end <= spin.end,
            `a call from ${JSON.stringify(call)} outside the spin ${JSON.stringify(spin)}`
          );
        }
      }
    });
  }
});

test(`objects keep their data and alarms over restarts with fewer threads ${on}`, async () => {
  // Every object has a value; most have an alarm due after both restarts, the others one that
  // fires on four threads.
  let directory;
  let pending;
  let early;
  const valueOf = (name) => `value-of-${name}`;
  const PENDING_PER_THREAD = 3;
  let due;
  await withServer(THREADS, async (server) => {
    directory = server.directory;
    const all = await threadsOf(server);
    const byHome = await homes(server, all, {
      prefix: 'restart',
      per: PENDING_PER_THREAD + 1,
    });
    pending = [...byHome.values()].flatMap((names) =>
      names.slice(0, PENDING_PER_THREAD)
    );
    early = [...byHome].map(([home, names]) => ({
      home,
      name: names[PENDING_PER_THREAD],
    }));
    due = Date.now() + 15_000;
    await Promise.all(
      [...pending, ...early.map(({ name }) => name)].map((name) =>
        get(server, `/put?name=${name}&value=${valueOf(name)}`)
      )
    );
    await Promise.all(
      pending.map((name) => get(server, `/alarm-at?name=${name}&at=${due}`))
    );
    await Promise.all(
      early.map(({ name }) => get(server, `/alarm?name=${name}&ms=200`))
    );
    for (const { home, name } of early) {
      let fired;
      for (let tries = 0; tries < TRIES; tries++) {
        fired = await get(server, `/alarms?name=${name}`);
        if (fired.alarms > 0) break;
        await scheduler.wait(20);
      }
      assert.deepStrictEqual(fired.isolates, [home]);
    }
    assert.deepStrictEqual(metadataFiles(directory), [
      'metadata-1.sqlite',
      'metadata-2.sqlite',
      'metadata-3.sqlite',
      'metadata.sqlite',
    ]);
  });

  // Each object's value and alarms, as a server finds them.
  const check = async (server, pendingAlarms) => {
    const all = await threadsOf(server);
    const states = await Promise.all(
      [...pending, ...early.map(({ name }) => name)].map(async (name) => ({
        name,
        value: (await get(server, `/value?name=${name}`)).value,
        alarms: await get(server, `/alarms?name=${name}`),
      }))
    );
    for (const { name, value, alarms } of states) {
      assert.strictEqual(value, valueOf(name));
      if (pending.includes(name)) {
        assert.strictEqual(alarms.alarms, pendingAlarms, name);
        for (const isolate of alarms.isolates) {
          assert(all.includes(isolate), `${name} fired in ${isolate}`);
        }
      } else {
        assert.strictEqual(alarms.alarms, 1, name);
      }
    }
  };

  await withServer(
    2,
    async (server) => {
      assert(Date.now() < due - 1000, 'the restarts took too long');
      await check(server, 0);
      const files = metadataFiles(directory);
      assert(!files.includes('metadata-2.sqlite'), `${files}`);
      assert(!files.includes('metadata-3.sqlite'), `${files}`);
      assert(files.includes('metadata.sqlite'), `${files}`);
    },
    { directory }
  );

  await withServer(
    1,
    async (server) => {
      assert(Date.now() < due, 'the restarts took too long');
      await check(server, 0);
      assert.deepStrictEqual(metadataFiles(directory), ['metadata.sqlite']);
      // Every pending alarm fires, once.
      await scheduler.wait(due - Date.now());
      for (const name of pending) {
        for (let tries = 0; ; tries++) {
          const { alarms } = await get(server, `/alarms?name=${name}`);
          if (alarms > 0) break;
          assert(tries < TRIES, `the alarm of ${name} did not fire`);
          await scheduler.wait(50);
        }
      }
      await scheduler.wait(1500);
      await check(server, 1);
    },
    { directory }
  );
});

// A refusal is the only way an attempt of a steered request may fail: one that fails otherwise
// fails the request, even if another attempt was answered.
test('steering fails on an attempt that fails otherwise than by a refusal', async () => {
  const server = { threads: THREADS, pending: 0 };
  let attempts = 0;
  await assert.rejects(
    steer(server, 'some isolate', async () => {
      attempts++;
      if (attempts === 1) return 'an answer';
      if (attempts === 2) throw new Error('the connection was reset');
      return undefined;
    }),
    /the connection was reset/
  );
  assert.strictEqual(server.pending, 0);
});
