// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, throws } from 'node:assert';
import trapModule from 'trap.wasm';

// Each service sets `expectedFrame` to how the trap's frame should name the module, and
// `childFlag` and `childFrame` for a loaded Worker given the compiled module as `lib/trap.wasm`.
export const wasmSourceUrlTest = {
  async test(ctrl, env) {
    const { exports } = new WebAssembly.Instance(trapModule);
    throws(
      () => exports.trap(),
      (error) => {
        ok(error.stack.includes(env.expectedFrame), error.stack);
        return true;
      }
    );

    const child = env.loader.load({
      compatibilityDate: '2025-01-01',
      compatibilityFlags: [env.childFlag],
      mainModule: 'main.js',
      modules: {
        'main.js': `
          import {WorkerEntrypoint} from "cloudflare:workers";
          import trapModule from './lib/trap.wasm';

          export default class extends WorkerEntrypoint {
            trapStack() {
              try {
                new WebAssembly.Instance(trapModule).exports.trap();
              } catch (e) {
                return e.stack;
              }
            }
          }
        `,
        'lib/trap.wasm': trapModule,
      },
    });
    const stack = await child.getEntrypoint().trapStack();
    ok(stack.includes(env.childFrame), stack);
  },
};
