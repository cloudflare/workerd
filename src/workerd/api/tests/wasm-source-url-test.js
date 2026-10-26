// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, throws } from 'node:assert';
import trapModule from 'trap.wasm';

// Each service sets `expectedFrame` to how the trap's frame should name the module.
export const wasmSourceUrlTest = {
  test(ctrl, env) {
    const { exports } = new WebAssembly.Instance(trapModule);
    throws(
      () => exports.trap(),
      (error) => {
        ok(error.stack.includes(env.expectedFrame), error.stack);
        return true;
      }
    );
  },
};
