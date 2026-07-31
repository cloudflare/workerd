// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, ok } from 'node:assert';
import processDefault from 'node:process';

export const getBuiltinModulePublicBuiltins = {
  test() {
    // process.getBuiltinModule must resolve known public built-ins to their
    // default export (the module value), so the type is whatever the module
    // exports -- e.g. node:assert's default export is the assert function.
    strictEqual(typeof process.getBuiltinModule('node:process'), 'object');
    strictEqual(typeof process.getBuiltinModule('node:buffer'), 'object');
    strictEqual(typeof process.getBuiltinModule('node:assert'), 'function');
  },
};

export const getBuiltinModuleReturnsDefaultExport = {
  test() {
    // For Node.js modules, getBuiltinModule must return the default export (the
    // module value), not the ES module namespace object. Regression test: the
    // new module registry path previously returned the namespace, so it was not
    // identical to the default import and carried a `default` property.
    const gbm = process.getBuiltinModule('node:process');
    strictEqual(gbm, processDefault);
    strictEqual(gbm, globalThis.process);
    strictEqual(typeof gbm.nextTick, 'function');
    ok(!('default' in gbm));
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
