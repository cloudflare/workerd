// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import process from 'node:process';

export const reportedMajorVersion = {
  test(_, env) {
    assert.match(process.versions.node, /^\d+\.\d+\.\d+$/);
    assert.strictEqual(process.version, `v${process.versions.node}`);
    assert.strictEqual(
      process.versions.node.split('.')[0],
      env.EXPECTED_MAJOR_VERSION
    );
  },
};
