// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

// These must match the limits hard-coded in worker-loader.c++.
const MAX_CODE_SIZE = 64 * 1024 * 1024; // 64 MiB
const MAX_ENV_SIZE = 1 * 1024 * 1024; // 1 MiB

const MAIN_MODULE = `
  import {WorkerEntrypoint} from "cloudflare:workers";
  export default class extends WorkerEntrypoint {
    ping() { return "pong"; }
    envBigLength() { return this.env.big ? this.env.big.length : 0; }
  }
`;

function makeCode(overrides) {
  return {
    compatibilityDate: '2025-01-01',
    mainModule: 'main.js',
    modules: { 'main.js': MAIN_MODULE },
    globalOutbound: null,
    ...overrides,
  };
}

// A worker whose total module size is comfortably under the limit loads and runs fine.
export let codeSizeWithinLimit = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('codeSizeWithinLimit', () =>
      makeCode({
        modules: {
          'main.js': MAIN_MODULE,
          // ~1 MiB of additional (uncompiled) module content, well under the limit.
          'pad.js': '// ' + 'x'.repeat(1 * 1024 * 1024),
        },
      })
    );

    assert.strictEqual(await worker.getEntrypoint().ping(), 'pong');
  },
};

// A worker whose total module size exceeds the limit fails to load with a clear error.
export let codeSizeExceedsLimit = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('codeSizeExceedsLimit', () =>
      makeCode({
        modules: {
          'main.js': MAIN_MODULE,
          // Push the total just over the limit. This module is never compiled because the size
          // check throws first.
          'big.js': '// ' + 'x'.repeat(MAX_CODE_SIZE),
        },
      })
    );

    await assert.rejects(worker.getEntrypoint().ping(), (e) => {
      assert.strictEqual(e.name, 'Error');
      assert.match(
        e.message,
        /^Dynamic Worker code size \(\d+ bytes\) exceeds the maximum allowed size of 67108864 bytes\.$/
      );
      return true;
    });
  },
};

// An env value under the limit is passed through to the dynamic worker.
export let envSizeWithinLimit = {
  async test(ctrl, env, ctx) {
    const big = 'x'.repeat(512 * 1024); // 512 KiB, under the 1 MiB limit.
    let worker = env.loader.get('envSizeWithinLimit', () =>
      makeCode({ env: { big } })
    );

    assert.strictEqual(await worker.getEntrypoint().envBigLength(), big.length);
  },
};

// An env value over the limit fails to load with a clear error.
export let envSizeExceedsLimit = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('envSizeExceedsLimit', () =>
      makeCode({ env: { big: 'x'.repeat(2 * MAX_ENV_SIZE) } })
    );

    await assert.rejects(worker.getEntrypoint().ping(), (e) => {
      assert.strictEqual(e.name, 'Error');
      assert.match(
        e.message,
        /^Dynamic Worker env size \(\d+ bytes\) exceeds the maximum allowed size of 1048576 bytes\.$/
      );
      return true;
    });
  },
};
