// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import reporters, { tap, spec, dot, junit, lcov } from 'node:test/reporters';

export const allReportersExistTest = {
  test() {
    for (const name of ['tap', 'spec', 'dot', 'junit', 'lcov']) {
      assert.strictEqual(
        typeof reporters[name],
        'function',
        `reporters.${name} missing`
      );
    }
    assert.strictEqual(typeof tap, 'function');
    assert.strictEqual(typeof spec, 'function');
    assert.strictEqual(typeof dot, 'function');
    assert.strictEqual(typeof junit, 'function');
    assert.strictEqual(typeof lcov, 'function');
  },
};

export const reportersThrowTest = {
  async test() {
    const empty = (async function* () {})();
    await assert.rejects(async () => {
      // eslint-disable-next-line no-unused-vars
      for await (const _ of tap(empty)) {
      }
    }, /not implemented/i);
  },
};
