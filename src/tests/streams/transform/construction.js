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

// DIVERGENCE: argument conversion order, every member logged. The spec
// (TypeScript) converts both strategies first, the writable then the
// readable (each highWaterMark and its ToNumber, then size), and only
// then reads the transformer's members, in lexicographic order. C++
// reads the transformer's members first, in its own order, then each
// strategy in that order. A transformer without and with a transform()
// hook covers the TS implementation's elided and standard construction
// paths.
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
    const strategies = [
      'writable.hwm',
      'writable.hwm.valueOf',
      'writable.size',
      'readable.hwm',
      'readable.hwm.valueOf',
      'readable.size',
    ];
    const members = (
      usingTsImpl
        ? [
            'cancel',
            'expectedLength',
            'flush',
            'readableType',
            'start',
            'transform',
            'writableType',
          ]
        : [
            'readableType',
            'writableType',
            'start',
            'transform',
            'flush',
            'cancel',
            'expectedLength',
          ]
    ).map((name) => `transformer.${name}`);
    for (const transform of [undefined, function transform() {}]) {
      const order = [];
      const transformer = {};
      for (const name of [
        'readableType',
        'writableType',
        'start',
        'transform',
        'flush',
        'cancel',
        'expectedLength',
      ]) {
        Object.defineProperty(transformer, name, {
          get() {
            order.push(`transformer.${name}`);
            return name === 'transform' ? transform : undefined;
          },
        });
      }
      new TransformStream(
        transformer,
        loggedStrategy(order, 'writable'),
        loggedStrategy(order, 'readable')
      );
      deepStrictEqual(
        order,
        usingTsImpl ? [...strategies, ...members] : [...members, ...strategies]
      );
    }
  },
};

// DIVERGENCE: a null transformer is accepted by C++, which converts it as
// a WebIDL dictionary (no transformer), but rejected by TypeScript (spec:
// the argument is typed `object`, which null is not). Every other
// primitive throws TypeError, and a function is accepted and its members
// read, in both (spec).
export const garbageTransformerValidation = {
  test() {
    const transformerError = {
      name: 'TypeError',
      message: 'transformer must be an object',
    };
    if (usingTsImpl) {
      throws(() => new TransformStream(null), transformerError);
    } else {
      new TransformStream(null);
    }
    for (const transformer of [42, 'x', true, 1n, Symbol('s')]) {
      throws(
        () => new TransformStream(transformer),
        usingTsImpl ? transformerError : TypeError
      );
    }
    let started = false;
    new TransformStream(
      Object.assign(function transformer() {}, {
        start() {
          started = true;
        },
      })
    );
    strictEqual(started, true);
  },
};

// DIVERGENCE: each strategy argument converts as a WebIDL dictionary in
// both (spec): a function's members are read, and every primitive except
// null throws TypeError (messages differ). A readable strategy without a
// highWaterMark (null or {}) differs: C++ applies the TransformStream
// default highWaterMark (0) only when the argument is omitted, so the
// readable side takes the ReadableStream default (1); TypeScript applies
// 0 to any strategy without one, as the spec's ExtractHighWaterMark does.
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
    const strategyError = usingTsImpl
      ? {
          name: 'TypeError',
          message: 'The queuing strategy must be an object.',
        }
      : TypeError;
    for (const primitive of [5, 'x', true, 1n, Symbol('s')]) {
      throws(() => desiredSizes(primitive, undefined), strategyError);
      throws(() => desiredSizes(undefined, primitive), strategyError);
    }
  },
};

// DIVERGENCE: which error wins when two are possible. Both reject a
// primitive transformer before reading the strategies, so its TypeError
// beats a throwing strategy getter (spec), and both convert the
// strategies before checking readableType, so a throwing writable
// strategy getter beats readableType: 'bytes' (spec). C++ converts each
// highWaterMark to an integer while converting the arguments, so an
// invalid writable highWaterMark (TypeError) beats readableType: 'bytes'
// and a throwing readable strategy getter. TypeScript follows the spec:
// both strategies are converted, then readableType is checked, then each
// highWaterMark's range, so readableType: 'bytes' and the readable
// strategy getter's error both beat an invalid writable highWaterMark.
// Both convert every transformer member before they check readableType
// (spec), so a non-callable start or flush (TypeError, messages differ)
// or a throwing start getter beats readableType: 'bytes', and both check
// every callback before either highWaterMark's range (spec).
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
      (e) => e === boom
    );
    throws(
      () =>
        new TransformStream({ readableType: 'bytes' }, { highWaterMark: -1 }),
      usingTsImpl ? readableTypeError : negativeHighWaterMark
    );
    throws(
      () => new TransformStream({}, { highWaterMark: -1 }, throwingStrategy),
      usingTsImpl ? (e) => e === boom : negativeHighWaterMark
    );
    const startError = {
      name: 'TypeError',
      message: usingTsImpl
        ? 'transformer.start must be a function'
        : "Incorrect type for the 'start' field on 'Transformer': the provided value is not of type 'function'.",
    };
    throws(
      () => new TransformStream({ readableType: 'bytes', start: 1 }),
      startError
    );
    throws(
      () =>
        new TransformStream({
          readableType: 'bytes',
          get start() {
            throw boom;
          },
        }),
      (e) => e === boom
    );
    throws(() => new TransformStream({ readableType: 'bytes', flush: 1 }), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'transformer.flush must be a function'
        : "Incorrect type for the 'flush' field on 'Transformer': the provided value is not of type 'function'.",
    });
    throws(
      () => new TransformStream({ start: 1 }, {}, { highWaterMark: -1 }),
      startError
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
