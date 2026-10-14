// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor argument handling. The init dictionary is required in both
// implementations (with different error text); a bag WITHOUT highWaterMark
// diverges: the spec (TypeScript) requires the member, while C++ accepts
// the bag and reports NaN. The value itself is an unrestricted double —
// NaN, infinities, negatives, and ToNumber coercions are stored verbatim
// in both.

import { strictEqual, ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const classes = [CountQueuingStrategy, ByteLengthQueuingStrategy];

export const initDictionaryIsRequired = {
  test() {
    for (const Ctor of classes) {
      throws(
        () => new Ctor(),
        (err) => {
          strictEqual(err.constructor, TypeError);
          strictEqual(
            err.message,
            usingTsImpl
              ? 'init must be an object'
              : `Failed to construct '${Ctor.name}': constructor parameter 1 ` +
                  "is not of type 'QueuingStrategyInit'."
          );
          return true;
        }
      );
    }
  },
};

// init converts as a WebIDL dictionary (parity): a function's members are
// read, and every other primitive throws TypeError (messages differ).
export const initDictionaryShapes = {
  test() {
    for (const Ctor of classes) {
      const init = Object.assign(function init() {}, { highWaterMark: 3 });
      strictEqual(new Ctor(init).highWaterMark, 3, Ctor.name);
      for (const bad of [5, 'x', true, 1n, Symbol('s')]) {
        throws(
          () => new Ctor(bad),
          usingTsImpl
            ? { name: 'TypeError', message: 'init must be an object' }
            : TypeError
        );
      }
    }
  },
};

export const missingHighWaterMarkDiverges = {
  test() {
    for (const Ctor of classes) {
      if (usingTsImpl) {
        throws(() => new Ctor({}), {
          name: 'TypeError',
          message: 'init.highWaterMark is required',
        });
      } else {
        const strategy = new Ctor({});
        ok(Number.isNaN(strategy.highWaterMark));
      }
    }
  },
};

export const highWaterMarkIsUnrestrictedDouble = {
  test() {
    for (const Ctor of classes) {
      // No validation: stored verbatim.
      ok(Number.isNaN(new Ctor({ highWaterMark: NaN }).highWaterMark));
      strictEqual(
        new Ctor({ highWaterMark: -Infinity }).highWaterMark,
        -Infinity
      );
      strictEqual(
        new Ctor({ highWaterMark: Infinity }).highWaterMark,
        Infinity
      );
      strictEqual(new Ctor({ highWaterMark: -5 }).highWaterMark, -5);
      // ToNumber coercion applies to strings and objects.
      strictEqual(new Ctor({ highWaterMark: '42' }).highWaterMark, 42);
      strictEqual(
        new Ctor({ highWaterMark: { valueOf: () => 3 } }).highWaterMark,
        3
      );
    }
  },
};

// A strategy dictionary whose members count their reads.
function countingStrategy(highWaterMark) {
  const reads = { highWaterMark: 0, size: 0 };
  const strategy = {
    get highWaterMark() {
      reads.highWaterMark++;
      return highWaterMark;
    },
    get size() {
      reads.size++;
      return undefined;
    },
  };
  return { reads, strategy };
}

// init.highWaterMark is read once, as WebIDL dictionary conversion does
// (parity); size is not a member of QueuingStrategyInit.
export const initHighWaterMarkReadOnce = {
  test() {
    for (const Strategy of [CountQueuingStrategy, ByteLengthQueuingStrategy]) {
      const { reads, strategy } = countingStrategy(4);
      const qs = new Strategy(strategy);
      strictEqual(reads.highWaterMark, 1, Strategy.name);
      strictEqual(reads.size, 0, Strategy.name);
      strictEqual(qs.highWaterMark, 4, Strategy.name);
    }
  },
};
