// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as httpMod from 'node:http';
import httpDefault from 'node:http';

export const httpSetGlobalProxyFromEnvTest = {
  test() {
    assert.strictEqual(typeof httpMod.setGlobalProxyFromEnv, 'function');
    assert.strictEqual(typeof httpDefault.setGlobalProxyFromEnv, 'function');

    // No-op: must not throw, returns undefined.
    assert.strictEqual(httpMod.setGlobalProxyFromEnv(), undefined);
    assert.strictEqual(httpDefault.setGlobalProxyFromEnv(), undefined);
  },
};
