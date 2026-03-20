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
