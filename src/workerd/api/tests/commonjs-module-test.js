// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { foo, bar, default as baz } from 'foo';
import * as fooModule from 'foo';
import { strictEqual } from 'node:assert';

export const test = {
  async test() {
    strictEqual(fooModule.default, baz);
    strictEqual(fooModule.foo, foo);
    strictEqual(fooModule.bar, undefined);
    strictEqual(foo, 1);
    strictEqual(bar, undefined);
    strictEqual(baz.foo, foo);

    try {
      // This dynamically imports a CommonJS module whose body calls require('../dep'),
      // exercising the CommonJS require() resolution error handling. The failure
      // shape differs by module registry: the legacy registry rejects the
      // specifier itself (TypeError), while the new registry resolves it as a
      // URL escaping the bundle root and reports the module as not found
      // (Error, matching Node's ERR_MODULE_NOT_FOUND which extends Error).
      await import('bad-require');
      throw new Error('bad-require should not resolve');
    } catch (err) {
      if (Cloudflare.compatibilityFlags.new_module_registry) {
        strictEqual(err.name, 'Error');
        strictEqual(err.message, 'Module not found: ../dep');
      } else {
        strictEqual(err.name, 'TypeError');
        strictEqual(err.message, 'Invalid module specifier "../dep".');
      }
    }
  },
};
