// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges on the strategy classes' own user hooks, identical in
// both implementations: a plain chunk's byteLength getter runs INSIDE the
// class size function (both implementations property-read plain objects),
// and the init bag's highWaterMark getter runs inside the constructor.
// Re-entering the API from either is safe; getter throws propagate
// unwrapped. The class size functions are receiver-agnostic.
//
// Re-entrancy of USER strategy size callbacks into stream controllers
// (enqueue/close from inside size during an enqueue) is stream-machinery
// behavior and lives in the readable/writable suites.

import { strictEqual, ok, throws } from 'node:assert';

const blqsSize = () => new ByteLengthQueuingStrategy({ highWaterMark: 1 }).size;

export const chunkGetterThrowPropagates = {
  test() {
    throws(
      () =>
        blqsSize()({
          get byteLength() {
            throw new RangeError('re');
          },
        }),
      { name: 'RangeError', message: 're' }
    );
  },
};

export const chunkGetterReentersSize = {
  test() {
    const size = blqsSize();
    strictEqual(
      size({
        get byteLength() {
          return size({ byteLength: 7 });
        },
      }),
      7
    );
  },
};

export const sizeIsReceiverAgnostic = {
  test() {
    const size = blqsSize();
    strictEqual(size.call(null, { byteLength: 3 }), 3);
    strictEqual(
      size.call(new CountQueuingStrategy({ highWaterMark: 1 }), {
        byteLength: 4,
      }),
      4
    );
    strictEqual(
      new CountQueuingStrategy({ highWaterMark: 1 }).size.call(null),
      1
    );
  },
};

export const initGetterReentersConstructor = {
  test() {
    let reentered = false;
    const strategy = new CountQueuingStrategy({
      get highWaterMark() {
        reentered = true;
        return new ByteLengthQueuingStrategy({ highWaterMark: 2 })
          .highWaterMark;
      },
    });
    ok(reentered);
    strictEqual(strategy.highWaterMark, 2);
  },
};
