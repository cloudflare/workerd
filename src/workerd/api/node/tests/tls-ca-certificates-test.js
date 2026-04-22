// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as tlsMod from 'node:tls';
import tlsDefault from 'node:tls';

export const tlsGetCACertificatesTest = {
  test() {
    assert.strictEqual(typeof tlsMod.getCACertificates, 'function');
    assert.strictEqual(typeof tlsDefault.getCACertificates, 'function');

    // Empty array default for all valid types (and the no-arg form).
    assert.deepStrictEqual(tlsMod.getCACertificates(), []);
    assert.deepStrictEqual(tlsMod.getCACertificates('bundled'), []);
    assert.deepStrictEqual(tlsMod.getCACertificates('extra'), []);
    assert.deepStrictEqual(tlsMod.getCACertificates('system'), []);
    assert.deepStrictEqual(tlsDefault.getCACertificates('bundled'), []);

    // Invalid type is validated.
    assert.throws(() => tlsMod.getCACertificates('bogus'), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
  },
};

export const tlsSetDefaultCACertificatesTest = {
  test() {
    assert.strictEqual(typeof tlsMod.setDefaultCACertificates, 'function');
    assert.strictEqual(typeof tlsDefault.setDefaultCACertificates, 'function');

    // No-op: must not throw on a valid array.
    assert.strictEqual(tlsMod.setDefaultCACertificates([]), undefined);
    assert.strictEqual(
      tlsMod.setDefaultCACertificates(['-----BEGIN CERTIFICATE-----']),
      undefined
    );

    // Non-array rejected.
    assert.throws(() => tlsMod.setDefaultCACertificates('not-an-array'), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    assert.throws(() => tlsDefault.setDefaultCACertificates(null), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
  },
};
