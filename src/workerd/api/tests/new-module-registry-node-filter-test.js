// Test that per-module Node.js feature flag filtering works with the new
// module registry. node:repl is behind enable_nodejs_repl_module which has
// no enable date before 2026-03-17. With the compat flag
// disable_nodejs_repl_module explicitly set, the module must not be available.

import { ok, rejects } from 'node:assert';

// node:buffer should always be available when nodejs_compat is enabled.
import { Buffer } from 'node:buffer';
ok(Buffer);

export const gatedModuleNotAvailable = {
  async test() {
    // With disable_nodejs_repl_module set in the wd-test config, importing
    // node:repl must fail regardless of compat date.
    await rejects(() => import('node:repl'), {
      message: /node:repl/,
    });
  },
};
