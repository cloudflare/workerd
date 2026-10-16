// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without workers_api_getters_setters_on_prototype, an event's srcElement
// reads its current target, not its target (the event suite's legacy cell
// has the per-instance layout itself): the target during dispatch and null
// once it ends, while target and currentTarget stay the target.

import { strictEqual } from 'node:assert';

export const legacySrcElementIsTheCurrentTarget = {
  test() {
    const target = new EventTarget();
    const event = new Event('x');
    let during;
    target.addEventListener('x', (e) => {
      during = [e.target, e.currentTarget, e.srcElement];
    });
    target.dispatchEvent(event);
    strictEqual(during.length, 3);
    for (const value of during) {
      strictEqual(value, target);
    }
    strictEqual(event.target, target);
    strictEqual(event.currentTarget, target);
    strictEqual(event.srcElement, null);
  },
};
