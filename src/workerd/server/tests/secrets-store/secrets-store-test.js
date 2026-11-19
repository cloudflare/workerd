// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `/workerd/secrets-store.js`, after Miniflare's Secrets Store tests
// (packages/miniflare/test/plugins/secret-store/index.spec.ts), which write
// secrets through the admin API.

import assert from 'node:assert';

const admin = (binding) => binding['SecretsStoreSecret::admin_api']();
const value = (binding) => binding.get().catch(() => null);

export const single = {
  async test(ctrl, env) {
    await assert.rejects(env.SECRET.get(), {
      message: 'Secret "secret_name" not found',
    });
    await (await admin(env.SECRET)).create('example');
    assert.strictEqual(await env.SECRET.get(), 'example');
  },
};

// One name in two stores is two secrets; two names in one store, too.
export const multiple = {
  async test(ctrl, env) {
    const all = () =>
      Promise.all([env.SECRET1, env.SECRET2, env.SECRET3].map(value));
    assert.deepStrictEqual(await all(), [null, null, null]);
    await (await admin(env.SECRET1)).create('example_a');
    await (await admin(env.SECRET2)).create('example_b');
    assert.deepStrictEqual(await all(), ['example_a', 'example_b', null]);
    await (await admin(env.SECRET3)).create('example_c');
    assert.deepStrictEqual(await all(), [
      'example_a',
      'example_b',
      'example_c',
    ]);
  },
};

// The Worker's `env:<secret_name>` wins over the stored value; null does not.
export const override = {
  async test(ctrl, env) {
    assert.strictEqual(await env.OVERRIDDEN.get(), 'from-env');
    await (await admin(env.OVERRIDDEN)).create('stored');
    assert.strictEqual(await env.OVERRIDDEN.get(), 'from-env');
    await (await admin(env.UNSET)).create('stored');
    assert.strictEqual(await env.UNSET.get(), 'stored');
  },
};

// The rest of the admin API, which Miniflare's tests leave out.
export const adminApi = {
  async test(ctrl, env) {
    const api = await admin(env.ADMIN);
    const id = await api.create('one');
    assert.match(id, /^[0-9a-f]{32}$/);
    assert.strictEqual(await api.get(id), 'admin');
    assert.strictEqual(await api.update('two', id), id);
    assert.strictEqual(await env.ADMIN.get(), 'two');
    const copy = await api.duplicate(id, 'copy');
    assert.match(copy, /^[0-9a-f-]{36}$/);
    const keys = (await api.list()).map((k) => [k.name, k.metadata.uuid]);
    assert.deepStrictEqual(keys, [
      ['admin', id],
      ['copy', copy],
    ]);
    await api.delete(id);
    assert.strictEqual(await value(env.ADMIN), null);
    for (const call of [
      () => api.get(id),
      () => api.update('three', id),
      () => api.duplicate(id, 'again'),
      () => api.delete(id),
    ]) {
      await assert.rejects(call(), { message: 'Secret not found' });
    }
  },
};
