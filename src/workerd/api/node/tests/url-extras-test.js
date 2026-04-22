// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as urlMod from 'node:url';
import urlDefault from 'node:url';

export const urlExtrasTest = {
  test() {
    assert.strictEqual(typeof urlMod.URLPattern, 'function');
    assert.strictEqual(urlDefault.URLPattern, urlMod.URLPattern);

    assert.strictEqual(typeof urlMod.fileURLToPathBuffer, 'function');
    assert.strictEqual(typeof urlDefault.fileURLToPathBuffer, 'function');

    const buf = urlMod.fileURLToPathBuffer('file:///tmp/foo');
    assert.ok(Buffer.isBuffer(buf));
    assert.strictEqual(buf.toString(), '/tmp/foo');
  },
};
