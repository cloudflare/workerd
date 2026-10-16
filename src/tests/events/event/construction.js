// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing Event and CustomEvent: argument conversion and a new
// event's attributes. CustomEvent's detail is custom-event.js's.
//
// Divergences (ledger in AGENTS.md):
// - #2: C++ target and srcElement are undefined before any dispatch.
// - #3: C++ currentTarget is undefined before any dispatch (null under
//   pedantic_wpt).
// - #4: C++ timeStamp is always 0.

import {
  deepStrictEqual,
  notStrictEqual,
  ok,
  strictEqual,
  throws,
} from 'node:assert';
import { usingTsImpl, pedanticWpt } from 'which-impl';

const kInitMembers = ['bubbles', 'cancelable', 'composed'];

export const typeArgumentIsRequired = {
  test() {
    throws(() => new Event(), TypeError);
    throws(() => new CustomEvent(), TypeError);
  },
};

// ToString, so a Symbol throws and a throwing toString() propagates.
export const typeIsConvertedToString = {
  test() {
    strictEqual(new Event(null).type, 'null');
    strictEqual(new Event(undefined).type, 'undefined');
    strictEqual(new Event(1).type, '1');
    strictEqual(new Event('').type, '');
    strictEqual(new CustomEvent({ toString: () => 'y' }).type, 'y');
    throws(() => new Event(Symbol('s')), TypeError);
    throws(() => new CustomEvent(Symbol('s')), TypeError);
    const error = new Error('toString');
    const type = {
      toString() {
        throw error;
      },
    };
    throws(
      () => new Event(type),
      (e) => e === error
    );
  },
};

// WebIDL dictionary conversion: undefined and null are an empty dictionary,
// any object (a function or an array too) is read, and every other
// primitive is a TypeError.
export const initDictionaryConversion = {
  test() {
    const withBubbles = () => {};
    withBubbles.bubbles = true;
    for (const ctor of [Event, CustomEvent]) {
      for (const init of [undefined, null, {}, [], () => {}]) {
        strictEqual(new ctor('x', init).bubbles, false);
      }
      strictEqual(new ctor('x', withBubbles).bubbles, true);
      for (const init of [true, 1, 'bubbles', Symbol('s'), 1n]) {
        throws(() => new ctor('x', init), TypeError, String(init));
      }
    }
  },
};

// The type is converted first; then each member is read once, the
// inherited EventInit members before detail.
export const initMembersReadOnceInOrder = {
  test() {
    for (const [ctor, expected] of [
      [Event, ['type', ...kInitMembers]],
      [CustomEvent, ['type', ...kInitMembers, 'detail']],
    ]) {
      const log = [];
      const init = {};
      for (const name of [...kInitMembers, 'detail']) {
        Object.defineProperty(init, name, {
          get() {
            log.push(name);
            return true;
          },
        });
      }
      const type = {
        toString() {
          log.push('type');
          return 't';
        },
      };
      new ctor(type, init);
      deepStrictEqual(log, expected, ctor.name);
    }
  },
};

export const initMembersAreConvertedToBoolean = {
  test() {
    const cases = [
      [undefined, false],
      [null, false],
      [0, false],
      ['', false],
      [NaN, false],
      [0n, false],
      [1, true],
      ['false', true],
      [{}, true],
      [Symbol('s'), true],
      [1n, true],
    ];
    for (const [value, expected] of cases) {
      const init = { bubbles: value, cancelable: value, composed: value };
      for (const event of [new Event('x', init), new CustomEvent('x', init)]) {
        for (const name of kInitMembers) {
          strictEqual(event[name], expected, `${name}: ${String(value)}`);
        }
      }
    }
  },
};

export const initMembersAreIndependent = {
  test() {
    for (const name of kInitMembers) {
      const event = new Event('x', { [name]: true });
      for (const other of kInitMembers) {
        strictEqual(event[other], other === name, `${name} sets ${other}`);
      }
    }
  },
};

// #2 and #3.
export const defaultAttributes = {
  test() {
    const event = new Event('x');
    strictEqual(event.type, 'x');
    strictEqual(event.bubbles, false);
    strictEqual(event.cancelable, false);
    strictEqual(event.composed, false);
    strictEqual(event.isTrusted, false);
    strictEqual(event.defaultPrevented, false);
    strictEqual(event.returnValue, true);
    strictEqual(event.cancelBubble, false);
    strictEqual(event.eventPhase, Event.NONE);
    deepStrictEqual(event.composedPath(), []);
    strictEqual(event.target, usingTsImpl ? null : undefined);
    strictEqual(event.srcElement, usingTsImpl ? null : undefined);
    strictEqual(
      event.currentTarget,
      usingTsImpl || pedanticWpt ? null : undefined
    );
  },
};

export const composedPathIsFresh = {
  test() {
    const event = new Event('x');
    const path = event.composedPath();
    notStrictEqual(event.composedPath(), path);
    path.push(1);
    deepStrictEqual(event.composedPath(), []);
  },
};

// #4. TS reads Date.now() as the event is constructed. Outside any request
// (here, as this module is evaluated) that is 0.
const timeStampAtModuleEvaluation = new Event('x').timeStamp;

export const timeStampIsDateNowAtConstruction = {
  async test() {
    strictEqual(timeStampAtModuleEvaluation, 0);
    const before = Date.now();
    const event = new Event('x');
    const custom = new CustomEvent('y');
    const after = Date.now();
    await scheduler.wait(10);
    const later = new Event('z');
    if (usingTsImpl) {
      for (const { timeStamp } of [event, custom]) {
        ok(
          before <= timeStamp && timeStamp <= after,
          `${timeStamp} is not in [${before}, ${after}]`
        );
      }
      ok(later.timeStamp > after, `${later.timeStamp} <= ${after}`);
    } else {
      for (const e of [event, custom, later]) {
        strictEqual(e.timeStamp, 0);
      }
    }
  },
};
