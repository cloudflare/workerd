// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import source mathWasm from './math.wasm';

// Test passing a WebAssembly.Module (from a source phase import) into the dynamic worker
// loader, where it can be imported with `import source` again.
export let wasmModuleSource = {
  async test(ctrl, env, ctx) {
    assert.ok(mathWasm instanceof WebAssembly.Module);

    for (let extraFlags of [[], ['new_module_registry']]) {
      let worker = env.loader.get(`wasmModuleSource-${extraFlags}`, () => {
        return {
          compatibilityDate: '2025-01-01',
          compatibilityFlags: extraFlags,
          allowExperimental: extraFlags.length > 0,
          mainModule: 'main.js',
          modules: {
            'main.js': `
              import {WorkerEntrypoint} from "cloudflare:workers";
              import source mathSource from './lib/math.wasm';
              import mathDefault from './lib/math.wasm';

              export default class extends WorkerEntrypoint {
                async getWasmAdd(a, b) {
                  if (!(mathSource instanceof WebAssembly.Module)) {
                    throw new Error("expected source phase import to be a WebAssembly.Module");
                  }
                  if (!(mathDefault instanceof WebAssembly.Module)) {
                    throw new Error("expected default import to be a WebAssembly.Module");
                  }
                  const instance = await WebAssembly.instantiate(mathSource);
                  return instance.exports.add(a, b);
                }
              }
            `,
            // Cover both accepted forms: the module directly, and `{ wasm: module }`.
            'lib/math.wasm': extraFlags.length ? { wasm: mathWasm } : mathWasm,
          },
        };
      });

      let entrypoint = worker.getEntrypoint();

      assert.strictEqual(await entrypoint.getWasmAdd(5, 7), 12);
      assert.strictEqual(await entrypoint.getWasmAdd(100, 42), 142);
    }
  },
};
