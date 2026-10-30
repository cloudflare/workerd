// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The stop propagation flags outside dispatch, through stopPropagation(),
// stopImmediatePropagation() and cancelBubble. How dispatch consults and
// clears them is event-target/dispatch.js's.
//
// Divergences (ledger in AGENTS.md):
// - #6: C++'s cancelBubble setter clears the flag on false; per spec,
//   setting false does nothing.
// - #7: C++'s stopImmediatePropagation() leaves cancelBubble false; per
//   spec, it sets the stop propagation flag too.
// - #8: C++'s cancelBubble setter takes a missing argument as false; WebIDL
//   setters throw a TypeError.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const stopPropagationSetsCancelBubble = {
  test() {
    const event = new Event('x');
    strictEqual(event.cancelBubble, false);
    event.stopPropagation();
    strictEqual(event.cancelBubble, true);
    event.stopPropagation();
    strictEqual(event.cancelBubble, true);
  },
};

// ToBoolean, and true stops propagation.
export const cancelBubbleSetter = {
  test() {
    const cases = [
      [true, true],
      ['yes', true],
      [1, true],
      [{}, true],
      [false, false],
      [0, false],
      ['', false],
      [null, false],
      [undefined, false],
    ];
    for (const [value, expected] of cases) {
      const event = new Event('x');
      event.cancelBubble = value;
      strictEqual(event.cancelBubble, expected, String(value));
    }
  },
};

// #6.
export const cancelBubbleFalseIsNoOp = {
  test() {
    const stopped = new Event('x');
    stopped.stopPropagation();
    stopped.cancelBubble = false;
    strictEqual(stopped.cancelBubble, usingTsImpl);

    const set = new Event('x');
    set.cancelBubble = true;
    set.cancelBubble = false;
    strictEqual(set.cancelBubble, usingTsImpl);
  },
};

// #7.
export const stopImmediatePropagationSetsCancelBubble = {
  test() {
    const event = new Event('x');
    event.stopImmediatePropagation();
    strictEqual(event.cancelBubble, usingTsImpl);
  },
};

// #8. Only a direct call can omit a setter's argument.
export const setterArity = {
  test() {
    const cancelBubble = Object.getOwnPropertyDescriptor(
      Event.prototype,
      'cancelBubble'
    ).set;
    const event = new Event('x');
    event.stopPropagation();
    if (!usingTsImpl) {
      cancelBubble.call(event);
      strictEqual(event.cancelBubble, false);
      return;
    }
    throws(() => cancelBubble.call(event), TypeError);
    strictEqual(event.cancelBubble, true);
    // returnValue's too; C++ has no returnValue setter (#5).
    const returnValue = Object.getOwnPropertyDescriptor(
      Event.prototype,
      'returnValue'
    ).set;
    const cancelable = new Event('x', { cancelable: true });
    throws(() => returnValue.call(cancelable), TypeError);
    strictEqual(cancelable.defaultPrevented, false);
  },
};
