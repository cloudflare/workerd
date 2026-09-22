// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor validation shapes. Complements WPT transform-streams/
// general.any.js and strategies.any.js, whose C++ expectedFailures here
// narrow to error TYPES (both implementations do validate).

import { throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// DIVERGENCE: transformer.readableType / writableType must be undefined;
// a defined value throws RangeError under TypeScript (spec) but
// TypeError under C++ (with a trailing period in the message).
export const readableWritableTypeValidation = {
  test() {
    for (const key of ['readableType', 'writableType']) {
      throws(() => new TransformStream({ [key]: 'bytes' }), {
        name: usingTsImpl ? 'RangeError' : 'TypeError',
        message: usingTsImpl
          ? `transformer.${key} must be undefined`
          : `transformer.${key} must be undefined.`,
      });
    }
    // undefined is valid everywhere.
    new TransformStream({ readableType: undefined, writableType: undefined });
  },
};

// DIVERGENCE (same family as the writable suite's ledger #1): an invalid
// highWaterMark on either strategy throws RangeError under TypeScript
// (spec) but TypeError from the C++ jsg uint64 boundary — for the
// writable AND the readable strategy alike.
export const highWaterMarkValidated = {
  test() {
    const expected = usingTsImpl ? RangeError : TypeError;
    for (const hwm of [-1, NaN]) {
      throws(
        () => new TransformStream({}, { highWaterMark: hwm }, {}),
        expected
      );
      throws(
        () => new TransformStream({}, {}, { highWaterMark: hwm }),
        expected
      );
    }
  },
};

// DIVERGENCE, and the root cause of most WPT reentrant-strategies (and
// several errors.any/strategies.any) C++ expectedFailures: a readable
// strategy with highWaterMark: Infinity is ACCEPTED by TypeScript (the
// spec allows any non-negative number) but REJECTED by the C++
// constructor's integer conversion. The WPT scenarios built on
// { highWaterMark: Infinity } therefore never construct the stream under
// C++ — re-probed with finite high-water marks, most of those behaviors
// are parity (see reentrancy.js).
export const hwmInfinityRejected = {
  test() {
    if (usingTsImpl) {
      new TransformStream(undefined, undefined, {
        highWaterMark: Infinity,
      });
      new TransformStream(undefined, { highWaterMark: Infinity }, undefined);
    } else {
      for (const args of [
        [undefined, undefined, { highWaterMark: Infinity }],
        [undefined, { highWaterMark: Infinity }, undefined],
      ]) {
        throws(() => new TransformStream(...args), {
          name: 'TypeError',
          message:
            'The value cannot be converted because it is not an integer.',
        });
      }
    }
  },
};
