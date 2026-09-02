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
