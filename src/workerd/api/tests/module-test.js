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

    // The failure shape differs by module registry: the legacy registry
    // rejects the escaping specifier itself (TypeError, with importer
    // context); the new registry resolves it as a URL escaping the bundle
    // root and reports the normalized URL as not found (Error, matching
    // Node's ERR_MODULE_NOT_FOUND which extends Error).
    if (Cloudflare.compatibilityFlags.new_module_registry) {
      await assert3.rejects(import('bad-static-import'), {
        name: 'Error',
        message: /Module not found: file:\/\/\/dep/,
      });
    } else {
      await assert3.rejects(import('bad-static-import'), {
        name: 'TypeError',
        message:
          /Invalid module specifier "\.\.\/dep"[\s\S]*imported from "bad-static-import"\./,
      });
    }
  },
};
