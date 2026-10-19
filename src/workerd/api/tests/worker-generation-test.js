// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

const child = `
  import { WorkerEntrypoint } from 'cloudflare:workers';
  export default class extends WorkerEntrypoint {
    snapshot() {
      return {
        stamp: Array.from(new Int32Array(this.env.stamp)),
        marks: Array.from(new Int32Array(this.env.marks)),
      };
    }
    exercise() {
      const stamp = new Int32Array(this.env.stamp);
      const marks = new Int32Array(this.env.marks);
      const active = Atomics.load(stamp, 0);
      const chain = Array.from(stamp.slice(2));
      try {
        Atomics.store(stamp, 0, 31);
        for (let i = 2; i < 9; ++i) Atomics.store(stamp, i, i < 5 ? i + 40 : 0);
        Atomics.store(marks, 0, 12);
        const getter = { get value() {
          Atomics.store(stamp, 1, 32);
          Atomics.store(marks, 1, 12);
          Atomics.store(marks, 2, 1);
          return Atomics.load(stamp, 1);
        }};
        const seen = getter.value;
        let sum = 0;
        for (let i = 0; i < 10000; ++i) sum += i;
        return { seen, sum, chain: Array.from(stamp.slice(2)) };
      } finally {
        Atomics.store(stamp, 0, active);
        for (let i = 0; i < 7; ++i) Atomics.store(stamp, i + 2, chain[i]);
      }
    }
    async resume() {
      const stamp = new Int32Array(this.env.stamp);
      await Promise.resolve();
      Atomics.store(stamp, 1, 33);
      return new Promise(resolve => queueMicrotask(() => {
        Atomics.store(stamp, 1, 34);
        resolve(Atomics.load(stamp, 1));
      }));
    }
    async park(ready) {
      const marks = new Int32Array(this.env.marks);
      const port = new MessageChannel();
      let announced = false;
      let resume;
      const announce = () => {
        if (!announced && Atomics.load(marks, 0) > 0 && Atomics.load(marks, 1) > 0) {
          announced = true;
          resume = ready().then(() => Atomics.store(marks, 2, 99));
          this.ctx.waitUntil(resume);
        }
      };
      port.port1.onmessage = () => {
        Atomics.add(marks, 0, 1);
        announce();
      };
      const timer = setInterval(() => {
        const count = Atomics.add(marks, 1, 1) + 1;
        port.port2.postMessage(count);
        announce();
        if (count === 10000) clearInterval(timer);
      }, 1);
      await new Promise(() => {});
      Atomics.store(marks, 2, 100);
    }
  }
`;

function code(generation, modules = child, extra = {}) {
  return {
    compatibilityDate: '2026-10-01',
    mainModule: 'main.js',
    modules: { 'main.js': modules },
    globalOutbound: null,
    allowExperimental: true,
    generation,
    ...extra,
  };
}

export const cellsAndFreshGeneration = {
  async test(ctrl, env) {
    const generation = env.loader.createGeneration({ stamp: 36, marks: 12 });
    const stamp = new Int32Array(generation.getSharedMemory('stamp'));
    const marks = new Int32Array(generation.getSharedMemory('marks'));
    assert.equal(stamp.length, 9);
    assert.equal(marks.length, 3);
    Atomics.store(stamp, 0, 7);
    for (let i = 2; i < 9; ++i) Atomics.store(stamp, i, i + 10);
    const worker = env.loader.load(code(generation));
    const entry = worker.getEntrypoint();
    assert.deepEqual((await entry.snapshot()).stamp, Array.from(stamp));
    assert.deepEqual(await entry.exercise(), {
      seen: 32,
      sum: 49995000,
      chain: [42, 43, 44, 0, 0, 0, 0],
    });
    assert.equal(Atomics.load(stamp, 0), 7);
    assert.deepEqual(Array.from(stamp.slice(2)), [12, 13, 14, 15, 16, 17, 18]);
    assert.deepEqual(Array.from(marks), [12, 12, 1]);
    assert.equal(await entry.resume(), 34);
    assert.equal(Atomics.load(stamp, 1), 34);
    let releases = 0;
    const release = generation.whenTerminated().then(() => ++releases);
    await Promise.all([
      generation.terminate(),
      generation.terminate(),
      release,
    ]);
    assert.equal(releases, 1);
    await generation.whenTerminated();
    assert.throws(() => worker.getEntrypoint(), /generation stopped/);
    await assert.rejects(entry.snapshot(), /generation stopped/);
    assert.throws(
      () => env.loader.load(code(generation)),
      /generation stopped/
    );

    const next = env.loader.createGeneration({ stamp: 36, marks: 12 });
    const freshStamp = new Int32Array(next.getSharedMemory('stamp'));
    const fresh = env.loader.load(code(next)).getEntrypoint();
    assert.deepEqual(Array.from(freshStamp), Array(9).fill(0));
    await generation.terminate();
    assert.deepEqual((await fresh.snapshot()).marks, [0, 0, 0]);
    Atomics.store(stamp, 0, 999);
    assert.equal(Atomics.load(freshStamp, 0), 0);
    await next.terminate();
  },
};

export const pendingResourcesStopBeforeAcknowledgement = {
  async test(ctrl, env) {
    const generation = env.loader.createGeneration({ stamp: 36, marks: 12 });
    const marks = new Int32Array(generation.getSharedMemory('marks'));
    const entry = env.loader.load(code(generation)).getEntrypoint();
    let signalStarted;
    const started = new Promise((resolve) => {
      signalStarted = resolve;
    });
    let resume;
    const afterStop = new Promise((resolve) => {
      resume = resolve;
    });
    generation.terminateAfter(5000);
    const pending = entry.park(() => {
      signalStarted();
      return afterStop;
    });
    const rejected = assert.rejects(pending, /generation stopped/);
    await Promise.race([
      started,
      scheduler.wait(2000).then(() => {
        throw new Error('resource callbacks never ran');
      }),
    ]);
    assert(Atomics.load(marks, 0) > 0 && Atomics.load(marks, 0) < 10000);
    assert(Atomics.load(marks, 1) > 0 && Atomics.load(marks, 1) < 10000);
    await generation.terminate();
    await rejected;
    const after = Array.from(marks);
    resume();
    await scheduler.wait(25);
    assert.deepEqual(Array.from(marks), after);
    assert.equal(Atomics.load(marks, 2), 0);
    await assert.rejects(entry.resume(), /generation stopped/);
  },
};

export const boundsAndOneShotStartup = {
  async test(ctrl, env) {
    assert.throws(
      () => env.loader.createGeneration({ tooLarge: 65537 }),
      /65536/
    );
    const generation = env.loader.createGeneration({ stamp: 36, marks: 12 });
    assert.throws(() => generation.getSharedMemory('absent'), /Unknown/);
    assert.throws(() => generation.terminateAfter(NaN), /deadline/);
    assert.throws(
      () => env.loader.load(code(generation, child, { env: { stamp: 1 } })),
      /conflicts/
    );
    const conflict = {};
    Object.defineProperty(conflict, 'stamp', {
      get() {
        throw new Error('collision getter must stay unread');
      },
    });
    assert.throws(
      () => env.loader.load(code(generation, child, { env: conflict })),
      /conflicts/
    );
    const enabledGet = env.loader.get('generation-get-is-invalid', () =>
      code(generation)
    );
    await assert.rejects(
      enabledGet.getEntrypoint().snapshot(),
      /only be attached with load/
    );
    const expected = new Error('env Proxy trap failed');
    const proxy = new Proxy(
      {},
      {
        getOwnPropertyDescriptor() {
          throw expected;
        },
      }
    );
    assert.throws(
      () => env.loader.load(code(generation, child, { env: proxy })),
      (error) => error === expected
    );
    assert.throws(
      () => env.loader.load(code(123)),
      /WorkerGenerationController|generation/
    );
    env.loader.load(code(generation));
    assert.throws(
      () => env.loader.load(code(generation)),
      /only be claimed once/
    );
    await generation.terminate();
    await generation.whenTerminated();
  },
};
