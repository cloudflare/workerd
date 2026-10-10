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

// This worker compiles without wasm_esm_integration_builtins, so the module passed to the
// dynamic worker was compiled without the builtins. The child enables the flag, so it must
// recompile from the wire bytes rather than share the parent's compiled code.
import source jsStringWasm from './js-string.wasm';
export let wasmModuleSourceBuiltinsMismatch = {
  async test(ctrl, env, ctx) {
    await assert.rejects(WebAssembly.instantiate(jsStringWasm, {}), {
      name: 'TypeError',
    });

    for (let extraFlags of [[], ['new_module_registry']]) {
      let worker = env.loader.get(`wasmBuiltinsMismatch-${extraFlags}`, () => {
        return {
          compatibilityDate: '2025-01-01',
          compatibilityFlags: ['wasm_esm_integration_builtins', ...extraFlags],
          allowExperimental: extraFlags.length > 0,
          mainModule: 'main.js',
          modules: {
            'main.js': `
              import {WorkerEntrypoint} from "cloudflare:workers";
              import source wasm from './js-string.wasm';

              export default class extends WorkerEntrypoint {
                async constantLength() {
                  const instance = await WebAssembly.instantiate(wasm, {});
                  return instance.exports.constantLength();
                }
              }
            `,
            'js-string.wasm': jsStringWasm,
          },
        };
      });

      assert.strictEqual(await worker.getEntrypoint().constantLength(), 11);
    }
  },
};
