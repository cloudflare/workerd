// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

export const nativeDeadlineStopsLoaderTurn = {
  async test(ctrl, env) {
    const generation = env.loader.createGeneration({ stamp: 36, marks: 12 });
    const stamp = new Int32Array(generation.getSharedMemory('stamp'));
    const worker = env.loader.load({
      compatibilityDate: '2026-10-01',
      mainModule: 'main.js',
      allowExperimental: true,
      generation,
      globalOutbound: null,
      modules: {
        'main.js': `
        import { WorkerEntrypoint } from 'cloudflare:workers';
        export default class extends WorkerEntrypoint {
          ready() { return true; }
          spin() {
            const cells = new Int32Array(this.env.stamp);
            Atomics.store(cells, 0, 1);
            for (let i = 0; i < 500000000; ++i) Atomics.store(cells, 1, i);
            Atomics.store(cells, 2, 1);
          }
        }
      `,
      },
    });
    const entry = worker.getEntrypoint();
    assert.equal(await entry.ready(), true);
    let acknowledged = false;
    const joined = generation.whenTerminated().then(() => {
      acknowledged = true;
    });
    await scheduler.wait(1);
    assert.equal(acknowledged, false);
    generation.terminateAfter(50);
    await assert.rejects(entry.spin(), /generation stopped/);
    await joined;
    assert.equal(acknowledged, true);
    assert.equal(Atomics.load(stamp, 0), 1);
    assert.equal(Atomics.load(stamp, 2), 0);
    const ended = Array.from(stamp);
    await scheduler.wait(10);
    assert.deepEqual(Array.from(stamp), ended);
    await assert.rejects(entry.ready(), /generation stopped/);
  },
};
