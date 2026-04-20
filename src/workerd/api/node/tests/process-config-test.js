// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import process from 'node:process';

export const processConfigTargetDefaultsTest = {
  test() {
    const td = process.config.target_defaults;
    assert.strictEqual(typeof td, 'object');
    assert.notStrictEqual(td, null);

    // Existing key still present.
    assert.strictEqual(td.default_configuration, 'Release');

    // New keys.
    assert.strictEqual(typeof td.configurations, 'object');
    assert.notStrictEqual(td.configurations, null);
    assert.ok(!Array.isArray(td.configurations));

    assert.ok(Array.isArray(td.libraries));
    assert.strictEqual(td.libraries.length, 0);

    assert.ok(Array.isArray(td.include_dirs));
    assert.strictEqual(td.include_dirs.length, 0);

    assert.ok(Array.isArray(td.defines));
    assert.strictEqual(td.defines.length, 0);

    assert.ok(Array.isArray(td.cflags));
    assert.strictEqual(td.cflags.length, 0);

    // All frozen.
    assert.ok(Object.isFrozen(td.configurations));
    assert.ok(Object.isFrozen(td.libraries));
    assert.ok(Object.isFrozen(td.include_dirs));
    assert.ok(Object.isFrozen(td.defines));
    assert.ok(Object.isFrozen(td.cflags));
  },
};

export const processConfigVariablesTest = {
  test() {
    const v = process.config.variables;
    assert.strictEqual(typeof v, 'object');
    assert.notStrictEqual(v, null);

    // Existing key still present.
    assert.strictEqual(v.napi_build_version, '9');

    // New array keys.
    assert.ok(Array.isArray(v.node_cctest_sources));
    assert.strictEqual(v.node_cctest_sources.length, 0);
    assert.ok(Object.isFrozen(v.node_cctest_sources));

    assert.ok(Array.isArray(v.node_library_files));
    assert.strictEqual(v.node_library_files.length, 0);
    assert.ok(Object.isFrozen(v.node_library_files));

    assert.ok(Array.isArray(v.node_builtin_shareable_builtins));
    assert.strictEqual(v.node_builtin_shareable_builtins.length, 0);
    assert.ok(Object.isFrozen(v.node_builtin_shareable_builtins));
  },
};
