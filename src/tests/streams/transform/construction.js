// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor validation shapes. Complements WPT transform-streams/
// general.any.js and strategies.any.js, whose C++ expectedFailures here
// narrow to error TYPES (both implementations do validate).

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
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

// DIVERGENCE: argument conversion order. The spec converts both
// strategies first, the writable then the readable (each highWaterMark
// and its ToNumber, then size), and only then reads the transformer's
// members. C++ reads the transformer's members first, then each strategy
// in that order. TypeScript reads the transformer's members first, then
// the writable strategy's size before its highWaterMark, and converts the
// readable strategy's highWaterMark after reading its size. An empty
// transformer and one with a transform() hook cover the TS
// implementation's elided and standard construction paths.
export const argumentConversionOrder = {
  test() {
    const loggedStrategy = (order, name) => ({
      get highWaterMark() {
        order.push(`${name}.hwm`);
        return {
          valueOf() {
            order.push(`${name}.hwm.valueOf`);
            return 1;
          },
        };
      },
      get size() {
        order.push(`${name}.size`);
        return undefined;
      },
    });
    for (const hooks of [{}, { transform() {} }]) {
      const order = [];
      new TransformStream(
        {
          get start() {
            order.push('transformer.start');
            return undefined;
          },
          ...hooks,
        },
        loggedStrategy(order, 'writable'),
        loggedStrategy(order, 'readable')
      );
      deepStrictEqual(
        order,
        usingTsImpl
          ? [
              'transformer.start',
              'writable.size',
              'writable.hwm',
              'writable.hwm.valueOf',
              'readable.hwm',
              'readable.size',
              'readable.hwm.valueOf',
            ]
          : [
              'transformer.start',
              'writable.hwm',
              'writable.hwm.valueOf',
              'writable.size',
              'readable.hwm',
              'readable.hwm.valueOf',
              'readable.size',
            ]
      );
    }
  },
};

// DIVERGENCE: C++ converts the transformer as a WebIDL dictionary: null
// means no transformer, a function is a transformer whose members are
// read, and every other primitive throws TypeError. TypeScript accepts
// null too, but throws TypeError for every other primitive and for a
// function. The spec rejects null and the other primitives (the argument
// is typed `object`) and accepts a function.
export const garbageTransformerValidation = {
  test() {
    const transformerError = {
      name: 'TypeError',
      message: 'transformer must be an object',
    };
    for (const transformer of [42, 'x', true, 1n, Symbol('s')]) {
      throws(
        () => new TransformStream(transformer),
        usingTsImpl ? transformerError : TypeError
      );
    }
    new TransformStream(null);
    let started = false;
    const transformer = Object.assign(function transformer() {}, {
      start() {
        started = true;
      },
    });
    if (usingTsImpl) {
      throws(() => new TransformStream(transformer), transformerError);
      strictEqual(started, false);
    } else {
      new TransformStream(transformer);
      strictEqual(started, true);
    }
  },
};

// DIVERGENCE: C++ converts each strategy argument as a WebIDL dictionary:
// null means a strategy with no members and every other primitive throws
// TypeError. TypeScript accepts null and every other primitive as no
// strategy, reading a primitive's members through its wrapper prototype.
// A readable strategy without a highWaterMark (null or {}) differs too:
// C++ applies the TransformStream default highWaterMark (0) only when the
// argument is omitted, so the readable side takes the ReadableStream
// default (1); TypeScript applies 0 to any strategy without one, as the
// spec's ExtractHighWaterMark does. Both read a function's members, as
// the spec does.
export const strategyArgumentShapes = {
  test() {
    // The writable's and the readable's desiredSize at construction.
    const desiredSizes = (writableStrategy, readableStrategy) => {
      let readable;
      const ts = new TransformStream(
        {
          start(c) {
            readable = c.desiredSize;
          },
        },
        writableStrategy,
        readableStrategy
      );
      return [ts.writable.getWriter().desiredSize, readable];
    };
    deepStrictEqual(desiredSizes(undefined, undefined), [1, 0]);
    for (const empty of [null, {}]) {
      deepStrictEqual(desiredSizes(empty, empty), [1, usingTsImpl ? 0 : 1]);
    }
    const strategy = Object.assign(function strategy() {}, {
      highWaterMark: 3,
    });
    deepStrictEqual(desiredSizes(strategy, strategy), [3, 3]);
    for (const primitive of [5, 'x', true, 1n, Symbol('s')]) {
      if (usingTsImpl) {
        deepStrictEqual(desiredSizes(primitive, undefined), [1, 0]);
        deepStrictEqual(desiredSizes(undefined, primitive), [1, 0]);
      } else {
        throws(() => desiredSizes(primitive, undefined), TypeError);
        throws(() => desiredSizes(undefined, primitive), TypeError);
      }
    }
  },
};

// DIVERGENCE: which error wins when two are possible. Both reject a
// primitive transformer before reading the strategies, so its TypeError
// beats a throwing strategy getter (spec). C++ converts all three
// arguments before validating anything, converting each highWaterMark
// to an integer: a throwing writable strategy getter beats readableType:
// 'bytes', and an invalid writable highWaterMark beats both readableType:
// 'bytes' and a throwing readable strategy getter. TypeScript checks
// readableType before reading either strategy, and range-checks the
// writable highWaterMark before reading the readable strategy. The spec
// converts both strategies first, then checks readableType, then each
// highWaterMark's range.
export const conversionErrorPrecedence = {
  test() {
    const boom = new Error('boom');
    const throwingStrategy = {
      get highWaterMark() {
        throw boom;
      },
    };
    const readableTypeError = {
      name: 'RangeError',
      message: 'transformer.readableType must be undefined',
    };
    const negativeHighWaterMark = {
      name: 'TypeError',
      message:
        'The value cannot be converted because it is negative and this API expects a positive bigint.',
    };
    throws(
      () => new TransformStream(42, throwingStrategy),
      usingTsImpl
        ? { name: 'TypeError', message: 'transformer must be an object' }
        : TypeError
    );
    throws(
      () => new TransformStream({ readableType: 'bytes' }, throwingStrategy),
      usingTsImpl ? readableTypeError : (e) => e === boom
    );
    throws(
      () =>
        new TransformStream({ readableType: 'bytes' }, { highWaterMark: -1 }),
      usingTsImpl ? readableTypeError : negativeHighWaterMark
    );
    throws(
      () => new TransformStream({}, { highWaterMark: -1 }, throwingStrategy),
      usingTsImpl
        ? { name: 'RangeError', message: 'Invalid highWaterMark' }
        : negativeHighWaterMark
    );
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

// Both strategy dictionaries' members are each read once, as WebIDL
// dictionary conversion does (parity). An empty transformer and one with
// a transform() hook cover the TS implementation's elided and standard
// construction paths.
export const strategyMembersReadOnce = {
  test() {
    for (const transformer of [{}, { transform() {} }]) {
      const writable = countingStrategy(2);
      const readable = countingStrategy(3);
      const ts = new TransformStream(
        transformer,
        writable.strategy,
        readable.strategy
      );
      deepStrictEqual(writable.reads, { highWaterMark: 1, size: 1 });
      deepStrictEqual(readable.reads, { highWaterMark: 1, size: 1 });
      strictEqual(ts.writable.getWriter().desiredSize, 2);
    }
  },
};
