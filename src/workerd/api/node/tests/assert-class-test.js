// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import * as assertMod from 'node:assert';
import { Assert as AssertNamed } from 'node:assert';
import strictDefault from 'node:assert/strict';
import * as strictMod from 'node:assert/strict';
import { Assert as AssertStrictNamed } from 'node:assert/strict';

export const assertAssertClassTest = {
  test() {
    assert.strictEqual(typeof assert.Assert, 'function');
    assert.strictEqual(assert.Assert, assertMod.Assert);
    assert.strictEqual(assert.Assert, AssertNamed);
    assert.strictEqual(assert.Assert.prototype.constructor, assert.Assert);

    assert.throws(() => new assert.Assert(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    assert.throws(() => new assert.Assert({ strict: true }), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};

export const assertStrictAssertClassTest = {
  test() {
    assert.strictEqual(assert.strict.Assert, assert.Assert);
    assert.strictEqual(strictDefault.Assert, assert.Assert);
    assert.strictEqual(strictMod.Assert, assert.Assert);
    assert.strictEqual(AssertStrictNamed, assert.Assert);
  },
};

// assert.CallTracker was removed from Node.js (DEP0173), so it is not exposed.
export const assertCallTrackerAbsentTest = {
  test() {
    assert.strictEqual(assert.CallTracker, undefined);
    assert.strictEqual(assertMod.CallTracker, undefined);
    assert.strictEqual(strictMod.CallTracker, undefined);
  },
};
