// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// CustomEvent's detail (ported from WPT's CustomEvent.html), and a
// CustomEvent as an Event.
//
// Divergence (ledger in AGENTS.md):
// - #10: C++ leaves detail undefined when the dictionary has none; per spec
//   it defaults to null.

import { ok, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// #10.
export const detailDefaultDiverges = {
  test() {
    const expected = usingTsImpl ? null : undefined;
    strictEqual(new CustomEvent('x').detail, expected);
    strictEqual(new CustomEvent('x', null).detail, expected);
    strictEqual(new CustomEvent('x', {}).detail, expected);
    strictEqual(new CustomEvent('x', { detail: undefined }).detail, expected);
  },
};

export const detailIsReturnedAsGiven = {
  test() {
    const object = {};
    const event = new CustomEvent('x', { detail: object });
    strictEqual(event.detail, object);
    strictEqual(event.detail, object);
    for (const detail of [null, 0, '', false, 'detail', 1n, Symbol.iterator]) {
      strictEqual(new CustomEvent('x', { detail }).detail, detail);
    }
  },
};

export const customEventIsAnEvent = {
  test() {
    const event = new CustomEvent('x', { cancelable: true, detail: 1 });
    ok(event instanceof CustomEvent);
    ok(event instanceof Event);
    event.preventDefault();
    strictEqual(event.defaultPrevented, true);

    const target = new EventTarget();
    let seen;
    target.addEventListener('y', (e) => {
      seen = e.detail;
    });
    strictEqual(
      target.dispatchEvent(new CustomEvent('y', { detail: 2 })),
      true
    );
    strictEqual(seen, 2);
  },
};
