// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as dnsMod from 'node:dns';
import dnsDefault from 'node:dns';
import * as dnsPromisesMod from 'node:dns/promises';
import dnsPromisesDefault from 'node:dns/promises';

export const dnsResolveTlsaTest = {
  test() {
    assert.strictEqual(typeof dnsMod.resolveTlsa, 'function');
    assert.strictEqual(typeof dnsDefault.resolveTlsa, 'function');

    assert.throws(() => dnsMod.resolveTlsa('example.com', () => {}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });

    // Resolver instance method.
    const resolver = new dnsMod.Resolver();
    assert.strictEqual(typeof resolver.resolveTlsa, 'function');
    assert.throws(() => resolver.resolveTlsa('example.com', () => {}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};

export const dnsPromisesResolveTlsaTest = {
  test: async () => {
    assert.strictEqual(typeof dnsPromisesMod.resolveTlsa, 'function');
    assert.strictEqual(typeof dnsPromisesDefault.resolveTlsa, 'function');
    // Also reachable via `dns.promises`.
    assert.strictEqual(typeof dnsMod.promises.resolveTlsa, 'function');

    await assert.rejects(dnsPromisesMod.resolveTlsa('example.com'), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    await assert.rejects(dnsMod.promises.resolveTlsa('example.com'), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};
