// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// V8 is built with Temporal, which it enables by default; workerd turns it off with
// --noharmony-temporal so the API surface stays unchanged.
export const temporalIsHidden = {
  test() {
    strictEqual(typeof Temporal, 'undefined');
    strictEqual('Temporal' in globalThis, false);
    strictEqual(Date.prototype.toTemporalInstant, undefined);
    strictEqual('toTemporalInstant' in Date.prototype, false);
  },
};
