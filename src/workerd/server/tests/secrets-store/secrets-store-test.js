// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `/workerd/secrets-store.js`, after Miniflare's Secrets Store tests
// (packages/miniflare/test/plugins/secret-store/index.spec.ts). Those write
// secrets through Miniflare's admin API; here each value is a binding of the
// Secrets Store Worker.

import assert from 'node:assert';

export const single = {
  async test(ctrl, env) {
    assert.strictEqual(await env.SECRET.get(), 'example');
  },
};

// Two names in one store are two secrets; one name in two stores is one.
export const multiple = {
  async test(ctrl, env) {
    const all = [env.SECRET1, env.SECRET2, env.SECRET3].map((s) => s.get());
    assert.deepStrictEqual(await Promise.all(all), [
      'example_a',
      'example_b',
      'example_a',
    ]);
  },
};

// A secret whose variable is unset, or that has none, is not found.
export const notFound = {
  async test(ctrl, env) {
    await assert.rejects(env.UNSET.get(), {
      message: 'Secret "unset" not found',
    });
    await assert.rejects(env.MISSING.get(), {
      message: 'Secret "missing" not found',
    });
  },
};
