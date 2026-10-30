// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

// jspi.wasm is built from jspi.wat: its export `run` returns what the import `env.wait` returns.
import MODULE from 'jspi.wasm';

export const jspiExists = {
  test() {
    assert.strictEqual(typeof WebAssembly.Suspending, 'function');
    assert.strictEqual(typeof WebAssembly.promising, 'function');
  },
};

export const suspendsAndResumes = {
  async test() {
    const wait = new WebAssembly.Suspending(async () => {
      await scheduler.wait(1);
      return 42;
    });
    const instance = new WebAssembly.Instance(MODULE, {
      env: { wait },
    });
    const run = WebAssembly.promising(instance.exports.run);
    const result = run();
    assert.ok(result instanceof Promise);
    assert.strictEqual(await result, 42);
  },
};

export const suspendingWithoutPromisingTraps = {
  test() {
    const wait = new WebAssembly.Suspending(async () => 1);
    const instance = new WebAssembly.Instance(MODULE, {
      env: { wait },
    });
    // Callbacks into the guest are not wrapped with promising, so a blocking import traps.
    assert.throws(() => instance.exports.run(), WebAssembly.SuspendError);
  },
};
