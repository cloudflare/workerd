// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Test that TypeScript modules work correctly with the new module registry
// when the typescript_strip_types compat flag is enabled. This exercises the
// transpiler path where the source is owned by a rust::String that may have
// shorter lifetime than the module registry.

import { strictEqual } from 'assert';
import { add, greet, PI } from 'ts-helper';

strictEqual(add(2, 3), 5);
strictEqual(greet('world'), 'hello, world');
strictEqual(PI, 3.14159);

export const dynamicImportTs = {
  async test() {
    const mod = await import('ts-helper');
    strictEqual(mod.add(10, 20), 30);
    strictEqual(mod.greet('test'), 'hello, test');
    strictEqual(mod.PI, 3.14159);
  },
};
