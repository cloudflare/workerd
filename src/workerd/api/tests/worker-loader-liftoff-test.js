// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import mathWasm from './math.wasm';

// The config passes --allow-natives-syntax.
function isLiftoffFunction(f) {
  return %IsLiftoffFunction(f);
}

// Starting a new isolate must not drop the Liftoff code of Wasm compiled by an existing one.
export const liftoffCodeSurvivesNewIsolate = {
  async test(ctrl, env) {
    const { exports } = new WebAssembly.Instance(mathWasm);
    assert.strictEqual(exports.add(1, 2), 3);
    assert.ok(isLiftoffFunction(exports.add));

    const worker = env.loader.get('child', () => ({
      compatibilityDate: '2025-01-01',
      mainModule: 'main.js',
      modules: {
        'main.js': `
          import { WorkerEntrypoint } from 'cloudflare:workers';
          export default class extends WorkerEntrypoint {
            ping() { return 'pong'; }
          }
        `,
      },
    }));
    assert.strictEqual(await worker.getEntrypoint().ping(), 'pong');

    assert.ok(isLiftoffFunction(exports.add));
  },
};
