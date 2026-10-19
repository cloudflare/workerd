// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The legacy initEvent() and initCustomEvent(), which re-run an event's
// initialization (ported from WPT's Event-initEvent.html).
//
// Divergence (ledger in AGENTS.md):
// - #9: C++ has neither method, which each test pins before stopping.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// The C++ side of #9: pins the absence, then stops the test.
function hasInitMethods() {
  if (usingTsImpl) return true;
  strictEqual('initEvent' in Event.prototype, false);
  strictEqual('initCustomEvent' in CustomEvent.prototype, false);
  return false;
}

export const initMethodsAreOperations = {
  test() {
    if (!hasInitMethods()) return;
    for (const [proto, name] of [
      [Event.prototype, 'initEvent'],
      [CustomEvent.prototype, 'initCustomEvent'],
    ]) {
      const { value, writable, enumerable, configurable } =
        Object.getOwnPropertyDescriptor(proto, name);
      strictEqual(value.name, name);
      strictEqual(value.length, 1);
      strictEqual(writable, true);
      strictEqual(enumerable, true);
      strictEqual(configurable, true);
    }
  },
};

// It sets type, bubbles and cancelable, unsets the canceled flag, and
// resets target; composed is kept.
export const initEventReinitializes = {
  test() {
    if (!hasInitMethods()) return;
    const target = new EventTarget();
    const event = new Event('x', { composed: true, cancelable: true });
    event.preventDefault();
    target.dispatchEvent(event);
    strictEqual(event.target, target);

    event.initEvent('y', true, true);
    strictEqual(event.type, 'y');
    strictEqual(event.bubbles, true);
    strictEqual(event.cancelable, true);
    strictEqual(event.composed, true);
    strictEqual(event.defaultPrevented, false);
    strictEqual(event.target, null);
    strictEqual(event.isTrusted, false);

    event.initEvent('z');
    strictEqual(event.type, 'z');
    strictEqual(event.bubbles, false);
    strictEqual(event.cancelable, false);
    strictEqual(event.composed, true);
  },
};

// It unsets the stop propagation flags, so a stopped event reaches
// listeners again.
export const initEventUnsetsStopFlags = {
  test() {
    if (!hasInitMethods()) return;
    for (const stop of ['stopPropagation', 'stopImmediatePropagation']) {
      const target = new EventTarget();
      let calls = 0;
      target.addEventListener('x', () => calls++);
      const event = new Event('x');
      event[stop]();
      event.initEvent('x');
      strictEqual(event.cancelBubble, false, stop);
      target.dispatchEvent(event);
      strictEqual(calls, 1, stop);
    }
  },
};

// It does nothing while the event is being dispatched, though its
// arguments are still converted.
export const initEventDuringDispatchIsNoOp = {
  test() {
    if (!hasInitMethods()) return;
    const target = new EventTarget();
    let conversions = 0;
    const type = {
      toString() {
        conversions++;
        return 'y';
      },
    };
    let seen;
    target.addEventListener('x', (e) => {
      e.initEvent(type, true, true);
      e.initCustomEvent(type, true, true, 2);
      seen = [e.type, e.bubbles, e.cancelable, e.detail, e.target === target];
    });
    target.dispatchEvent(new CustomEvent('x', { detail: 1 }));
    deepStrictEqual(seen, ['x', false, false, 1, true]);
    strictEqual(conversions, 2);
  },
};

export const initEventArgumentConversion = {
  test() {
    if (!hasInitMethods()) return;
    const event = new Event('x');
    throws(() => event.initEvent(), TypeError);
    throws(() => event.initEvent(Symbol('s')), TypeError);
    throws(() => Event.prototype.initEvent.call({}, 'x'), TypeError);
    event.initEvent(undefined);
    strictEqual(event.type, 'undefined');
    event.initEvent('y', 1, '');
    strictEqual(event.bubbles, true);
    strictEqual(event.cancelable, false);
    event.initEvent('z', undefined, {});
    strictEqual(event.bubbles, false);
    strictEqual(event.cancelable, true);
  },
};

// initCustomEvent() is initEvent() plus detail, which defaults to null;
// initEvent() leaves detail alone.
export const initCustomEventSetsDetail = {
  test() {
    if (!hasInitMethods()) return;
    const event = new CustomEvent('x', { detail: 1 });
    event.initCustomEvent('y');
    strictEqual(event.type, 'y');
    strictEqual(event.detail, null);
    const detail = {};
    event.initCustomEvent('z', true, true, detail);
    strictEqual(event.detail, detail);
    strictEqual(event.bubbles, true);
    strictEqual(event.cancelable, true);
    event.initEvent('w');
    strictEqual(event.detail, detail);
    throws(() => event.initCustomEvent(), TypeError);
    throws(
      () => CustomEvent.prototype.initCustomEvent.call(new Event('x'), 'y'),
      TypeError
    );
  },
};
