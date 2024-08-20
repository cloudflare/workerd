// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { openDoor } from 'test:module';

export const test_module_api = {
  test() {
    assert.throws(() => openDoor('test key'));
    assert.equal(openDoor('0p3n s3sam3'), true);
  },
};

export const test_builtin_dynamic_import = {
  async test() {
    await assert.doesNotReject(import('test:module'));
  },
};

// internal modules can't be imported
export const test_builtin_internal_dynamic_import = {
  async test() {
    await assert.rejects(import('test-internal:internal-module'));
  },
};

export const test_wrapped_binding = {
  async test(ctr, env) {
    assert.ok(env.door, 'binding is not present');
    assert.equal(typeof env.door, 'object');
    assert.ok(env.door.tryOpen);
    assert.equal(typeof env.door.tryOpen, 'function');

    // binding uses a different secret specified in the config
    assert.ok(env.door.tryOpen('open sesame'));
    assert.ok(!env.door.tryOpen('bad secret'));

    // check there are no other properties available
    assert.deepEqual(Object.keys(env.door), ['tryOpen']);
    assert.deepEqual(Object.getOwnPropertyNames(env.door), ['tryOpen']);

    assert.ok(env.customDoor, 'custom binding is not present');
    assert.ok(!env.customDoor.tryOpen('open sesame'));
    assert.ok(env.customDoor.tryOpen('custom open sesame'));
  },
};
