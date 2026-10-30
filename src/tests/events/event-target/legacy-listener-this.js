// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without set_event_target_this, a function listener is called with the
// global as `this`, not the target. An object listener's handleEvent still
// gets the object.

import { deepStrictEqual } from 'node:assert';

export const legacyListenerThis = {
  test() {
    const target = new EventTarget();
    const seen = [];
    target.addEventListener('x', function () {
      seen.push(this === globalThis);
    });
    const object = {
      handleEvent() {
        seen.push(this === object);
      },
    };
    target.addEventListener('x', object);
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(seen, [true, true]);
  },
};
