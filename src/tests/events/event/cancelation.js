// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Canceling an event outside dispatch: preventDefault(), defaultPrevented
// and returnValue. How dispatch reads the canceled flag is
// event-target/dispatch.js's.
//
// Divergence (ledger in AGENTS.md):
// - #5: C++ returnValue has no setter, so assigning it throws in strict code
//   (module code always is); TS's setter cancels, as preventDefault() does.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const preventDefaultNeedsCancelable = {
  test() {
    const cancelable = new Event('x', { cancelable: true });
    cancelable.preventDefault();
    strictEqual(cancelable.defaultPrevented, true);
    strictEqual(cancelable.returnValue, false);
    cancelable.preventDefault();
    strictEqual(cancelable.defaultPrevented, true);

    const plain = new Event('x');
    plain.preventDefault();
    strictEqual(plain.defaultPrevented, false);
    strictEqual(plain.returnValue, true);
  },
};

// #5.
export const returnValueSetterCancels = {
  test() {
    const event = new Event('x', { cancelable: true });
    if (!usingTsImpl) {
      throws(() => {
        event.returnValue = false;
      }, TypeError);
      strictEqual(event.defaultPrevented, false);
      return;
    }
    event.returnValue = true;
    strictEqual(event.defaultPrevented, false);
    event.returnValue = false;
    strictEqual(event.defaultPrevented, true);
    strictEqual(event.returnValue, false);
    // Setting true does not uncancel.
    event.returnValue = true;
    strictEqual(event.defaultPrevented, true);

    // ToBoolean.
    const coerced = new Event('x', { cancelable: true });
    coerced.returnValue = 'false';
    strictEqual(coerced.defaultPrevented, false);
    coerced.returnValue = 0;
    strictEqual(coerced.defaultPrevented, true);

    // Like preventDefault(), it needs a cancelable event.
    const plain = new Event('x');
    plain.returnValue = false;
    strictEqual(plain.defaultPrevented, false);
    strictEqual(plain.returnValue, true);
  },
};
