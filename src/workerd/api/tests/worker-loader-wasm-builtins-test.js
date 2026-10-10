// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import source jsStringWasm from './js-string.wasm';

// This worker compiles with wasm_esm_integration_builtins, so the module passed to the dynamic
// worker was compiled with the builtins. The child does not enable the flag, so it must not
// share the parent's compiled code.
export let wasmModuleSourceBuiltinsDisabledInChild = {
  async test(ctrl, env, ctx) {
    const instance = await WebAssembly.instantiate(jsStringWasm, {});
    assert.strictEqual(instance.exports.constantLength(), 11);

    for (let extraFlags of [[], ['new_module_registry']]) {
      let worker = env.loader.get(`wasmBuiltinsDisabled-${extraFlags}`, () => {
        return {
          compatibilityDate: '2025-01-01',
          compatibilityFlags: extraFlags,
          allowExperimental: extraFlags.length > 0,
          mainModule: 'main.js',
          modules: {
            'main.js': `
              import {WorkerEntrypoint} from "cloudflare:workers";
              import source wasm from './js-string.wasm';

              export default class extends WorkerEntrypoint {
                async instantiate() {
                  try {
                    await WebAssembly.instantiate(wasm, {});
                  } catch (e) {
                    return e.name;
                  }
                  return 'ok';
                }
              }
            `,
            'js-string.wasm': jsStringWasm,
          },
        };
      });

      assert.strictEqual(
        await worker.getEntrypoint().instantiate(),
        'TypeError'
      );
    }
  },
};
