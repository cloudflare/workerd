// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

// Tests for Event behavior that is enabled by the pedantic_wpt compatibility flag.

export const returnValueSetter = {
  test() {
    const target = new EventTarget();

    const cancelable = new Event('foo', { cancelable: true });
    cancelable.returnValue = true;
    strictEqual(cancelable.defaultPrevented, false);
    strictEqual(cancelable.returnValue, true);
    cancelable.returnValue = false;
    strictEqual(cancelable.defaultPrevented, true);
    strictEqual(cancelable.returnValue, false);
    // Setting it back to true does not clear the canceled flag.
    cancelable.returnValue = true;
    strictEqual(cancelable.defaultPrevented, true);

    const nonCancelable = new Event('foo');
    nonCancelable.returnValue = false;
    strictEqual(nonCancelable.defaultPrevented, false);
    strictEqual(nonCancelable.returnValue, true);

    const listener = (event) => {
      event.returnValue = false;
    };
    target.addEventListener('foo', listener);
    strictEqual(
      target.dispatchEvent(new Event('foo', { cancelable: true })),
      false
    );
    strictEqual(target.dispatchEvent(new Event('foo')), true);
    target.removeEventListener('foo', listener);

    target.addEventListener('foo', listener, { passive: true });
    const passiveEvent = new Event('foo', { cancelable: true });
    strictEqual(target.dispatchEvent(passiveEvent), true);
    strictEqual(passiveEvent.defaultPrevented, false);
  },
};
