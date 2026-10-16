// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without dont_substitute_null_on_type_error, a primitive listener fails
// conversion and is taken as null, so addEventListener() and
// removeEventListener() do nothing with it. An object without a callable
// handleEvent still throws, as it does with the flag.

import { strictEqual, throws } from 'node:assert';

export const legacyPrimitiveListenersAreIgnored = {
  test() {
    const target = new EventTarget();
    for (const listener of [1, 's', false, true]) {
      target.addEventListener('x', listener);
      target.removeEventListener('x', listener);
    }
    strictEqual(target.dispatchEvent(new Event('x')), true);
    throws(() => target.addEventListener('x', {}), TypeError);
  },
};
