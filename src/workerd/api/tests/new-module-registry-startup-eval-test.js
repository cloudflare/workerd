// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, strictEqual, throws } from 'node:assert';
import wasm from 'wasm';

// The static wasm import above is compiled lazily during the top-level
// evaluation of this module graph, inside the startup window where the
// allow_eval_during_startup compat flag permits dynamic code evaluation.
// That permission must survive the compilation: eval() and new Function()
// must still work in top-level code that runs after the import.
strictEqual(eval('6 * 7'), 42);
strictEqual(new Function('return 6 * 7')(), 42);
ok(wasm instanceof WebAssembly.Module);

export const evalPermittedDuringStartupOnly = {
  test() {
    // Startup succeeded (the top-level assertions above ran). At request time
    // the startup window has closed, so dynamic code evaluation is forbidden
    // again.
    throws(() => eval('1 + 1'), EvalError);
    throws(() => new Function('return 1'), EvalError);
  },
};
