// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Generic on<type> properties.
//
// Divergence (ledger in AGENTS.md):
// - #12: C++ takes a callable on<type> property of a target (its own, or
//   inherited from a subclass) as a listener for <type>, called before the
//   target's listeners with the global as `this`. Per spec only an event
//   handler attribute an interface declares is one, and EventTarget
//   declares none.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// #12.
export const onTypeProperty = {
  test() {
    const target = new EventTarget();
    const log = [];
    target.addEventListener('foo', () => {
      log.push('listener');
    });
    target.onfoo = function (event) {
      log.push(['onfoo', this === globalThis, event.type]);
    };
    strictEqual(target.dispatchEvent(new Event('foo')), true);
    deepStrictEqual(
      log,
      usingTsImpl ? ['listener'] : [['onfoo', true, 'foo'], 'listener']
    );
  },
};

// #12.
export const onTypeMethod = {
  test() {
    const log = [];
    class Target extends EventTarget {
      onfoo() {
        log.push('onfoo');
      }
    }
    new Target().dispatchEvent(new Event('foo'));
    deepStrictEqual(log, usingTsImpl ? [] : ['onfoo']);
  },
};

export const nonCallableOnTypeIsIgnored = {
  test() {
    const target = new EventTarget();
    for (const value of [1, 'handler', {}, null]) {
      target.onfoo = value;
      strictEqual(target.dispatchEvent(new Event('foo')), true);
    }
  },
};
