// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// setTimeout(), setInterval() and clearTimeout() (IoContext::TimeoutManagerImpl), and Date.now()
// with timers pending, whose earliest deadline clamps the clock. The Durable Object cases run
// inside blockConcurrencyWhile(), where timers are queued per critical section; each of them
// makes one RPC call, so they loop over many operations per call.
//
// A timer cleared before it fires keeps its entry in the IoContext's timeout map until the
// context is destroyed, and both contexts here live for the whole run, so later cases run with
// more dead entries than earlier ones. Compare a case across runs, not with other cases.
//
// The Durable Object clock cases run first: after the other Durable Object cases they made a
// debug build's run take about 25 times as long, for a reason not yet understood.
import { DurableObject } from 'cloudflare:workers';

const noop = () => {};
const IN_SECTION = 1000;

function setAndClear(count) {
  for (let i = 0; i < count; i++) clearTimeout(setTimeout(noop, 1000));
}

// Clearing in deadline order removes the queue's head each time, so every clear re-arms.
function setStaggeredAndClear(count) {
  const ids = new Array(count);
  for (let i = 0; i < count; i++) ids[i] = setTimeout(noop, 1000 + i);
  for (let i = 0; i < count; i++) clearTimeout(ids[i]);
}

function fireAll(count) {
  const fired = new Array(count);
  for (let i = 0; i < count; i++) {
    fired[i] = new Promise((resolve) => setTimeout(resolve, 0));
  }
  return Promise.all(fired);
}

function fireInterval(count) {
  return new Promise((resolve) => {
    let ticks = 0;
    const id = setInterval(() => {
      if (++ticks === count) {
        clearInterval(id);
        resolve();
      }
    }, 0);
  });
}

function setPending(count) {
  const ids = new Array(count);
  for (let i = 0; i < count; i++) ids[i] = setTimeout(noop, 60_000 + i);
  return ids;
}

function clearAll(ids) {
  for (const id of ids) clearTimeout(id);
}

function readClock(count) {
  let sum = 0;
  for (let i = 0; i < count; i++) sum += Date.now();
  return sum;
}

export class TimerBench extends DurableObject {
  setAndClear(count) {
    return this.ctx.blockConcurrencyWhile(async () => setAndClear(count));
  }

  setStaggeredAndClear(count) {
    return this.ctx.blockConcurrencyWhile(async () =>
      setStaggeredAndClear(count)
    );
  }

  async fireAll(count) {
    await this.ctx.blockConcurrencyWhile(() => fireAll(count));
  }

  readClock(count, pending) {
    return this.ctx.blockConcurrencyWhile(async () => {
      const ids = setPending(pending);
      const sum = readClock(count);
      clearAll(ids);
      return sum;
    });
  }
}

export default {
  bench(b, env) {
    b.run('setTimeout + clearTimeout', () => {
      clearTimeout(b.blackBox(setTimeout(noop, 1000)));
    });
    b.run('100 staggered setTimeout, then clear in order', () =>
      setStaggeredAndClear(100)
    );
    b.run(
      'setTimeout(0), awaited',
      () => new Promise((resolve) => setTimeout(resolve, 0))
    );
    b.run('100 setTimeout(0), all fired', () => fireAll(100));
    b.run('setInterval(0), 100 ticks', () => fireInterval(100));

    for (const pending of [0, 1, 1000]) {
      let ids;
      b.run(
        `Date.now() with ${pending} pending`,
        () => b.blackBox(Date.now()),
        {
          setup: () => (ids = setPending(pending)),
          teardown: () => clearAll(ids),
        }
      );
    }

    const stub = env.ns.get(env.ns.idFromName('timers'));
    for (const pending of [0, 1000]) {
      b.run(
        `critical section: ${IN_SECTION} Date.now() with ${pending} pending`,
        async () => b.blackBox(await stub.readClock(IN_SECTION, pending))
      );
    }
    b.run(`critical section: ${IN_SECTION} setTimeout + clearTimeout`, () =>
      stub.setAndClear(IN_SECTION)
    );
    b.run(
      `critical section: ${IN_SECTION} staggered, then clear in order`,
      () => stub.setStaggeredAndClear(IN_SECTION)
    );
    b.run(`critical section: ${IN_SECTION} setTimeout(0), all fired`, () =>
      stub.fireAll(IN_SECTION)
    );
  },
};
