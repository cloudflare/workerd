// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

export let jitlessStartup = {
  async test(controller, env, ctx) {
    assert.strictEqual(
      typeof WebAssembly,
      'undefined',
      'expected no WebAssembly global under --jitless'
    );

    assert.strictEqual(
      new TextDecoder().decode(new TextEncoder().encode('ok')),
      'ok'
    );
  },
};
