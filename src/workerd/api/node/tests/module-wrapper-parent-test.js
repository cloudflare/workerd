// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as moduleMod from 'node:module';
import moduleDefault from 'node:module';

export const moduleWrapperTest = {
  test() {
    assert.ok(Array.isArray(moduleDefault.wrapper), 'wrapper is an array');
    assert.strictEqual(moduleDefault.wrapper.length, 2);
    assert.strictEqual(typeof moduleDefault.wrapper[0], 'string');
    assert.strictEqual(typeof moduleDefault.wrapper[1], 'string');
    assert.ok(moduleDefault.wrapper[0].includes('exports'));
    // Named export mirror.
    assert.ok(Array.isArray(moduleMod.wrapper));

    // module.prototype.parent is a deprecated property; must exist and be
    // undefined (not missing — feature-detection check relies on the key).
    assert.ok(
      'parent' in moduleDefault.prototype,
      'Module.prototype.parent key exists'
    );
    assert.strictEqual(moduleDefault.prototype.parent, undefined);
  },
};
