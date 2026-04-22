// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as events from 'node:events';

export const eventsInitTest = {
  test() {
    assert.strictEqual(typeof events.init, 'function');
    // Hook is a no-op; must not throw and must return undefined.
    assert.strictEqual(events.init(), undefined);
  },
};
