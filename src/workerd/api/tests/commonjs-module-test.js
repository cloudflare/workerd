import { foo, bar, default as baz } from 'foo';
import * as fooModule from 'foo';
import { strictEqual } from 'node:assert';

export const test = {
  test() {
    strictEqual(fooModule.default, baz);
    strictEqual(fooModule.foo, foo);
    strictEqual(fooModule.bar, undefined);
    strictEqual(foo, 1);
    strictEqual(bar, undefined);
    strictEqual(baz.foo, foo);
  }
};
