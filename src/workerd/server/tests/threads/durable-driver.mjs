// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Durable Objects under `workerd serve --threads`: each object runs on its home thread, which
// the process places it on, and a request another thread serves reaches it there. The same
// checks run on four threads and on one. `placement-driver.mjs` has the cases about where objects
// live and what crosses between their threads; `durable-harness.mjs` says how a test has the
// thread it chooses serve a request.
import { test } from 'node:test';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';
import {
  SIGTERM_DRAINS,
  assertCleanEnd,
  THREADS,
  TRIES,
  UUID,
  body,
  get,
  homes,
  open,
  others,
  serve,
  threadsOf,
  webSocket,
  withServer,
} from './durable-harness.mjs';

for (const threads of [THREADS, 1]) {
  const on = `on ${threads} thread${threads > 1 ? 's' : ''}`;

  test(`requests of every thread count in one object ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      const PER_THREAD = 40;
      // A few at a time: a request for one thread is sent in several attempts, each over a
      // connection of its own, and a Windows client runs out of buffers for many at once.
      const AT_ONCE = 8;
      const answers = [];
      for (const front of all) {
        for (let sent = 0; sent < PER_THREAD; sent += AT_ONCE) {
          const batch = Array.from({ length: AT_ONCE }, async () => {
            const answer = await get(server, '/count?name=shared', front);
            assert.strictEqual(answer.front, front);
            return answer;
          });
          answers.push(...(await Promise.all(batch)));
        }
      }
      // One object, in the isolate of one of the threads, whichever thread asked.
      const object = answers[0].isolate;
      assert(all.includes(object), `the object runs in ${object}`);
      for (const answer of answers) assert.strictEqual(answer.isolate, object);
      const total = all.length * PER_THREAD;
      assert.deepStrictEqual(
        answers.map((a) => a.count).sort((a, b) => a - b),
        Array.from({ length: total }, (_, i) => i + 1)
      );
    }));

  test(`a stub does what stubs do ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      // For each front an object of its own, homed on another thread, where there is one.
      const byHome = await homes(server, all, {
        prefix: 'features',
        per: all.length,
      });
      const cases = all.map((front, i) => {
        const home = others(all, front)[0];
        return { front, home, name: byHome.get(home)[i] };
      });
      const results = await Promise.all(
        cases.map(({ front, name }) =>
          get(server, `/features?name=${name}`, front)
        )
      );
      for (const [i, result] of results.entries()) {
        const { front, home, name } = cases[i];
        const { homes } = result;
        for (const home of Object.values(homes)) {
          assert(all.includes(home), `an object runs in ${home}`);
        }
        assert.strictEqual(homes.counter, home);
        const at = { isolate: home };
        assert.match(result.service.front, UUID);
        assert.deepStrictEqual(result, {
          front,
          // The name the id was made from reaches the object.
          name,
          homes,
          // A stub the object returned, called in order.
          session: { sums: [1, 3], ...at },
          // Calls on one stub arrive in the order they were made.
          recorded: Array.from({ length: 50 }, (_, n) => n),
          // A stub passed to the object names the same object there, which runs at its own
          // home.
          other: { count: 1, isolate: homes.other },
          otherAgain: { count: 2, isolate: homes.other },
          // A service binding passed to the object is the binding of the object's thread.
          service: result.ownService,
          ownService: result.ownService,
          frontService: result.frontService,
          // An entrypoint with props, passed to the object, runs on the object's thread.
          greeter: { greeting: 'hello, object', ...at },
          // A function passed to the object runs where it was made.
          callBack: { object: homes.counter, front },
          facet: [
            { bumps: 1, ...at },
            { bumps: 2, ...at },
            { bumps: 1, ...at },
          ],
          error: { name: 'RangeError', message: 'the object refused' },
          ephemeral: { count: 2, isolate: homes.ephemeral },
          exported: { count: 2, isolate: homes.exported },
          connect: `object ${homes.counter}`,
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
        // The binding of the object's thread is not the one of the front's.
        if (threads > 1) {
          assert.notStrictEqual(
            result.service.front,
            result.frontService.front
          );
        }
      }
    }));

  test(`bodies stream to an object and back ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      const { isolate: home } = await get(server, '/where?name=echo');
      const front = others(all, home)[0];
      // The response head comes before any of the body was sent.
      const { req, res } = await open(server, '/echo?name=echo', front, {
        method: 'POST',
      });
      assert.strictEqual(res.statusCode, 200);
      assert.strictEqual(res.headers['x-front'], front);
      assert.strictEqual(res.headers['x-object'], home);
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
    }));

  test(`an alarm set through a stub fires once ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      const { isolate: home } = await get(server, '/where?name=alarm');
      const [front, later] = [...others(all, home), home];
      await get(server, '/alarm?name=alarm&ms=300', front);
      await scheduler.wait(1500);
      const fired = await get(server, '/alarms?name=alarm', front);
      assert.deepStrictEqual(fired, { front, alarms: 1, isolates: [home] });
      await scheduler.wait(1000);
      const after = await get(server, '/alarms?name=alarm', later);
      assert.deepStrictEqual(after, {
        front: later,
        alarms: 1,
        isolates: [home],
      });
    }));

  test(`the hooks that end every object reach every thread before they resolve ${on}`, () =>
    withServer(threads, async (server) => {
      const all = await threadsOf(server);
      // An object on each thread.
      const byHome = await homes(server, all, { prefix: 'hooked' });
      const objects = [...byHome].map(([home, [name]]) => ({ home, name }));
      const names = objects.map(({ name }) => name).join(',');
      // Has each object store `value` and run: the instance each runs as, by name.
      const prepare = async (value) => {
        const instances = new Map();
        await Promise.all(
          objects.map(async ({ name, home }) => {
            await get(server, `/put?name=${name}&value=${value}`);
            const where = await get(server, `/where?name=${name}`);
            assert.strictEqual(where.isolate, home);
            instances.set(name, where.instance);
          })
        );
        return instances;
      };
      // From each thread, a hook ends every object, which the request then calls at once: each
      // is a new instance at its home, with its storage as the hook leaves it.
      for (const hook of ['abort', 'delete', 'evict-all']) {
        for (const front of all) {
          const value = `${hook}-${front}`;
          const before = await prepare(value);
          const after = await get(
            server,
            `/hook?hook=${hook}&names=${names}`,
            front
          );
          assert.strictEqual(after.error, null);
          for (const [i, { home, name }] of objects.entries()) {
            const object = after.objects[i];
            assert.strictEqual(object.isolate, home);
            assert.notStrictEqual(
              object.instance,
              before.get(name),
              `${hook} from ${front} left the object on ${home} running`
            );
            assert.strictEqual(object.value, hook === 'delete' ? null : value);
          }
        }
      }
      // The hook of one stub, from each thread but the object's home: that object is a new
      // instance, the others are left running.
      for (const { home, name } of objects) {
        for (const front of others(all, home)) {
          const before = await prepare(`evict-${front}`);
          const after = await get(
            server,
            `/hook?hook=evict&name=${name}&names=${names}`,
            front
          );
          assert.strictEqual(after.error, null);
          for (const [i, object] of after.objects.entries()) {
            assert.strictEqual(object.isolate, objects[i].home);
            assert.strictEqual(object.value, `evict-${front}`);
            const wasEvicted = objects[i].name === name;
            assert.strictEqual(
              object.instance !== before.get(objects[i].name),
              wasEvicted,
              `evict of ${name} from ${front}: ${objects[i].name}`
            );
          }
        }
      }
      // A thread that runs JavaScript ends its objects only once it is free, so a hook that
      // another thread calls meanwhile resolves only after that.
      if (threads > 1) {
        const [front, busy] = all;
        for (const hook of ['abort', 'delete', 'evict-all']) {
          const before = await prepare(`busy-${hook}`);
          const { res } = await open(server, '/spin-body?ms=1500', busy);
          const spun = body(res);
          // The spin starts once the body is read.
          await scheduler.wait(100);
          const after = await get(
            server,
            `/hook?hook=${hook}&names=${names}`,
            front
          );
          const spin = JSON.parse((await spun).toString());
          assert.strictEqual(after.error, null);
          assert(
            after.calledAt > spin.start && after.calledAt < spin.end,
            `${hook} was called at ${after.calledAt}, not during the spin ${spin.start}-${spin.end}`
          );
          assert(
            after.resolvedAt >= spin.end,
            `${hook} resolved ${spin.end - after.resolvedAt} ms before the busy thread was free`
          );
          for (const [i, { home, name }] of objects.entries()) {
            assert.strictEqual(after.objects[i].isolate, home);
            assert.notStrictEqual(after.objects[i].instance, before.get(name));
          }
        }
      }
    }));

  test(
    `SIGTERM completes the object calls in flight ${on}`,
    SIGTERM_DRAINS,
    async () => {
      const server = await serve(threads);
      try {
        const all = await threadsOf(server);
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
        // Every other attempt to steer them has been refused.
        await server.quiet(all.length);
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
}

test('an idle object is shut down and a hibernating WebSocket wakes it', () =>
  withServer(THREADS, async (server) => {
    const all = await threadsOf(server);
    // Where each object lives, and a thread other than that.
    const where = async (name) => {
      const { isolate } = await get(server, `/where?name=${name}`);
      return { home: isolate, away: others(all, isolate) };
    };
    const plainAt = await where('plain');
    const hibernatingAt = await where('hibernating');
    const idleAt = await where('idle');

    // A WebSocket the object holds itself.
    const plain = await webSocket(server, '/ws?name=plain', plainAt.away[0]);
    assert.strictEqual(plain.front, plainAt.away[0]);
    plain.send('one');
    const one = await plain.next();
    assert.deepStrictEqual(
      { echo: one.echo, messages: one.messages, isolate: one.isolate },
      { echo: 'one', messages: 1, isolate: plainAt.home }
    );
    plain.send('two');
    assert.deepStrictEqual(await plain.next(), {
      ...one,
      echo: 'two',
      messages: 2,
    });

    // A WebSocket the object hibernates with, which a thread other than the object's took.
    const hibernating = await webSocket(
      server,
      '/ws?name=hibernating&hibernate',
      hibernatingAt.away.at(-1)
    );
    assert.strictEqual(hibernating.front, hibernatingAt.away.at(-1));
    hibernating.send('before');
    const before = await hibernating.next();
    assert.strictEqual(before.echo, 'before');
    assert.strictEqual(before.isolate, hibernatingAt.home);

    // An object nothing holds: the stub its request used is gone with the request.
    const idle = await get(server, '/where?name=idle', idleAt.away[0]);
    assert.strictEqual(idle.isolate, idleAt.home);

    // Past the ten seconds after which an idle object is shut down.
    await scheduler.wait(11500);

    // Each was constructed again: nothing on another thread kept the first instance alive.
    const again = await get(server, '/where?name=idle', idleAt.away[0]);
    assert.notStrictEqual(again.instance, idle.instance);
    assert.strictEqual(again.isolate, idleAt.home);

    assert(!hibernating.isClosed());
    hibernating.send('after');
    const after = await hibernating.next();
    assert.deepStrictEqual(
      { echo: after.echo, messages: after.messages, isolate: after.isolate },
      { echo: 'after', messages: 1, isolate: hibernatingAt.home }
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
  }));
