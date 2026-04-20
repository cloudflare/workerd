// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as tlsMod from 'node:tls';
import tlsDefault from 'node:tls';

export const tlsGetCACertificatesTest = {
  test() {
    assert.strictEqual(typeof tlsMod.getCACertificates, 'function');
    assert.strictEqual(tlsDefault.getCACertificates, tlsMod.getCACertificates);

    assert.deepStrictEqual(tlsMod.getCACertificates(), []);
    for (const type of ['default', 'bundled', 'extra', 'system']) {
      assert.deepStrictEqual(tlsMod.getCACertificates(type), []);
    }

    assert.throws(() => tlsMod.getCACertificates('bogus'), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
    assert.throws(() => tlsMod.getCACertificates(1), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
  },
};

export const tlsSetDefaultCACertificatesTest = {
  test() {
    assert.strictEqual(typeof tlsMod.setDefaultCACertificates, 'function');
    assert.strictEqual(
      tlsDefault.setDefaultCACertificates,
      tlsMod.setDefaultCACertificates
    );

    assert.throws(() => tlsMod.setDefaultCACertificates([]), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};
