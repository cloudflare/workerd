// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'a/b/c';
import * as assert3 from 'node:assert';

export const basics = {
  async test() {
    const assert2 = await import('a/b/c');
    if (assert !== assert2 && assert !== assert3) {
      throw new Error('bad things happened');
    }

    try {
      await import('bad-static-import');
      throw new Error('bad-static-import should not resolve');
    } catch (err) {
      assert3.strictEqual(err.name, 'TypeError');
      assert3.match(err.message, /Invalid module specifier "\.\.\/dep"/);
      assert3.match(err.message, /imported from "bad-static-import"\./);
    }
  },
};
