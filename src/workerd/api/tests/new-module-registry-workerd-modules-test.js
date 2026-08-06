// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import unsafeEval from 'workerd:unsafe-eval';
import unsafe from 'workerd:unsafe';
import rtti from 'workerd:rtti';
import { ok, strictEqual } from 'node:assert';

export const workerdModules = {
  async test() {
    // The workerd:unsafe-eval module is functional, not just resolvable: its
    // eval() grants dynamic code evaluation for the duration of the call,
    // even at request time.
    strictEqual(unsafeEval.eval('6 * 7'), 42);

    // The other workerd: modules expose their expected APIs.
    strictEqual(typeof unsafe.isTestAutogateEnabled, 'function');
    strictEqual(typeof rtti.exportTypes, 'function');

    // Dynamic import at request time resolves to the same per-context
    // instances as the static imports above.
    const dynamicUnsafeEval = await import('workerd:unsafe-eval');
    strictEqual(dynamicUnsafeEval.default, unsafeEval);
    const dynamicRtti = await import('workerd:rtti');
    strictEqual(dynamicRtti.default, rtti);

    // Outside an unsafeEval call, dynamic code evaluation stays forbidden.
    ok(
      (() => {
        try {
          eval('1 + 1');
          return false;
        } catch (e) {
          return e instanceof EvalError;
        }
      })()
    );
  },
};
