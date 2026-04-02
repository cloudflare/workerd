// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

export const getBuiltinModulePublicBuiltins = {
  test() {
    // process.getBuiltinModule must be able to resolve known public built-ins.
    strictEqual(typeof process.getBuiltinModule('node:process'), 'object');
    strictEqual(typeof process.getBuiltinModule('node:buffer'), 'object');
    strictEqual(typeof process.getBuiltinModule('node:assert'), 'object');
  },
};

export const getBuiltinModuleNonExistent = {
  test() {
    // Non-existent modules return undefined.
    strictEqual(process.getBuiltinModule('node:nonexistent'), undefined);
    strictEqual(process.getBuiltinModule('non-existent'), undefined);
  },
};

export const getBuiltinModuleInternalNotExposed = {
  test() {
    // Internal modules must not be accessible via getBuiltinModule.
    // These are BUILTIN_ONLY modules that should only be importable
    // by other built-in modules, never by user code.
    strictEqual(process.getBuiltinModule('node-internal:process'), undefined);
    strictEqual(
      process.getBuiltinModule('node-internal:public_process'),
      undefined
    );
    strictEqual(
      process.getBuiltinModule('node-internal:legacy_process'),
      undefined
    );
    strictEqual(
      process.getBuiltinModule('node-internal:internal_timers_global_override'),
      undefined
    );
    strictEqual(process.getBuiltinModule('cloudflare-internal:env'), undefined);
    strictEqual(
      process.getBuiltinModule('cloudflare-internal:filesystem'),
      undefined
    );
  },
};
