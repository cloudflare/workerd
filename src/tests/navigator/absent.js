// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without global_navigator there is neither a `navigator` nor a Navigator,
// whichever implementation the cell runs.

import { strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

export default {
  test() {
    strictEqual(typeof env.USING_TS_IMPL, 'boolean');
    strictEqual(typeof navigator, 'undefined');
    strictEqual(typeof Navigator, 'undefined');
    strictEqual('navigator' in globalThis, false);
    strictEqual('Navigator' in globalThis, false);
    strictEqual(globalThis.navigator, undefined);
  },
};
