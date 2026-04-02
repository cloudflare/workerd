// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
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
  },
};
