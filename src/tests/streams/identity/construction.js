// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor argument handling for IdentityTransformStream and
// FixedLengthStream.
//
// Both implementations convert a FixedLengthStream length as the C++
// implementation converts any uint64_t: a bigint must fit in a uint64;
// anything else goes through ToNumber (an object's valueOf runs once), must
// be finite and non-negative, and is truncated. A length above 2^53-1 is
// then rejected, after the strategy has been read. Every failure is a
// TypeError.
//
// The highWaterMark conversion diverges (both classes): C++ converts
// to a uint64 (TypeError for non-finite or negative values, fractions
// truncate, a bigint converts) and narrows desiredSize to a 32-bit int;
// TypeScript applies the WHATWG writable conversion (RangeError for NaN or
// negative values, Infinity and fractions kept, TypeError for a bigint).

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// FixedLengthStream does not expose expectedLength directly, but the coerced
// value is observable through the highWaterMark cap: the writer's initial
// desiredSize is min(expectedLength, highWaterMark).
function coercedLength(value) {
  return new FixedLengthStream(value, {
    highWaterMark: 1000,
  }).writable.getWriter().desiredSize;
}

export const identityConstruction = {
  test() {
    // Test happy-path construction for IdentityTransformStream
    ok(new IdentityTransformStream());
    ok(new IdentityTransformStream(undefined));
    ok(new IdentityTransformStream({}));
    ok(new IdentityTransformStream({ highWaterMark: 10 }));
  },
};

export const identityBrandChecks = {
  test() {
    const its = new IdentityTransformStream();

    // Intentional divergence: The C++ implementation has IdentityTransformStream
    // inherit directly from TransformStream, while the TypeScript implementation
    // does not. The TypeScript impl follows the established convention from
    // Web Platform Apis like CompressionStream and TextEncoderStream which
    // are transforms but do not inherit from TransformStream
    if (usingTsImpl) {
      ok(!(its instanceof TransformStream));
    } else {
      ok(its instanceof TransformStream);
    }

    const fls = new FixedLengthStream(10);
    ok(fls instanceof IdentityTransformStream);

    if (usingTsImpl) {
      ok(!(fls instanceof TransformStream));
    } else {
      ok(fls instanceof TransformStream);
    }
  },
};

export const fixedLengthValidLengths = {
  test() {
    ok(new FixedLengthStream(0));
    ok(new FixedLengthStream(5));
    // Negative zero is coerced to zero.
    ok(new FixedLengthStream(-0.0));
    ok(new FixedLengthStream(Number.MAX_SAFE_INTEGER));
    // bigint lengths are accepted.
    ok(new FixedLengthStream(0n));
    ok(new FixedLengthStream(100n));
    // With a queuing strategy.
    ok(new FixedLengthStream(3, { highWaterMark: 2 }));

    // The received length is observable via the highWaterMark cap.
    strictEqual(coercedLength(5), 5);
    strictEqual(coercedLength(100n), 100);
    // A -0.0 expectedLength converts to +0 in both implementations.
    // strictEqual is SameValue, so a leaked negative zero would fail here.
    strictEqual(coercedLength(-0.0), 0);
  },
};

export const fixedLengthInvalidLengths = {
  test() {
    // Every invalid length is a TypeError in both implementations: a
    // missing one, a negative or non-finite number, a bigint outside the
    // uint64 range, and a value that does not convert to a number.
    throws(() => new FixedLengthStream(), TypeError);
    for (const value of [
      -1,
      -1n,
      -0.5,
      NaN,
      Infinity,
      2n ** 64n,
      'abc',
      {},
      Symbol(),
    ]) {
      throws(() => new FixedLengthStream(value), TypeError);
    }
  },
};

export const fixedLengthLengthCoercion = {
  test() {
    // A length goes through ToNumber and is truncated toward zero.
    let valueOfCalls = 0;
    const object = {
      valueOf() {
        valueOfCalls++;
        return 4;
      },
    };
    strictEqual(coercedLength(0.5), 0);
    strictEqual(coercedLength(2.9), 2);
    strictEqual(coercedLength('10'), 10);
    strictEqual(coercedLength(null), 0);
    strictEqual(coercedLength(true), 1);
    strictEqual(coercedLength(object), 4);
    strictEqual(valueOfCalls, 1);
  },
};

export const fixedLengthLengthsAboveMaxSafeInteger = {
  test() {
    // A length above 2^53-1 is rejected once both arguments have
    // converted: the strategy is read first.
    const reads = [];
    const strategy = {
      get highWaterMark() {
        reads.push('highWaterMark');
        return 1;
      },
    };
    throws(() => new FixedLengthStream(2 ** 53, strategy), {
      name: 'TypeError',
      message:
        'FixedLengthStream requires an integer expected length less than 2^53.',
    });
    deepStrictEqual(reads, ['highWaterMark']);
    throws(() => new FixedLengthStream(2 ** 53), TypeError);
    throws(() => new FixedLengthStream(2n ** 53n), TypeError);
    throws(() => new FixedLengthStream(0xffff_ffff_ffff_ffffn), TypeError);
  },
};

export const userStrategySizeNeverInvoked = {
  async test() {
    // Both implementations consult only `highWaterMark` on the queuing
    // strategy; a user-supplied `size` callback is never invoked — with or
    // without an explicit highWaterMark — and plays no part in chunk
    // accounting. (C++ reads highWaterMark alone off the strategy bag; the
    // TypeScript implementation replaces the strategy with its own internal
    // size callback.)
    let calls = 0;
    for (const strategy of [
      { size: () => ++calls },
      { highWaterMark: 4, size: () => ++calls },
    ]) {
      for (const stream of [
        new IdentityTransformStream(strategy),
        new FixedLengthStream(3, strategy),
      ]) {
        const writer = stream.writable.getWriter();
        const reader = stream.readable.getReader();
        const write = writer.write(new Uint8Array([1, 2, 3]));
        deepStrictEqual([...(await reader.read()).value], [1, 2, 3]);
        await write;
      }
    }
    strictEqual(calls, 0);
  },
};

// The strategy argument converts as a WebIDL dictionary: undefined and
// null mean no strategy, and any other non-object throws a TypeError (the
// message differs per implementation).
export const strategyArgumentDictionaryConversion = {
  test() {
    for (const strategy of [undefined, null]) {
      for (const stream of [
        new IdentityTransformStream(strategy),
        new FixedLengthStream(5, strategy),
      ]) {
        strictEqual(stream.writable.getWriter().desiredSize, 1);
      }
    }
    for (const strategy of [5, 'x', true, 3n, Symbol('s')]) {
      throws(() => new IdentityTransformStream(strategy), TypeError);
      throws(() => new FixedLengthStream(5, strategy), TypeError);
    }
  },
};

function desiredSizeOrError(make) {
  try {
    return make().writable.getWriter().desiredSize;
  } catch (e) {
    return e.constructor.name;
  }
}

// highWaterMark is read once and converted once (ToNumber), by both
// classes. FixedLengthStream caps the converted number at its length.
export const highWaterMarkReadAndConvertedOnce = {
  test() {
    for (const [make, capped] of [
      [(strategy) => new IdentityTransformStream(strategy), Infinity],
      [(strategy) => new FixedLengthStream(5, strategy), 5],
      [(strategy) => new FixedLengthStream(1e6, strategy), 1e6],
    ]) {
      let reads = 0;
      let conversions = 0;
      const strategy = {
        get highWaterMark() {
          reads++;
          return {
            valueOf() {
              conversions++;
              return 4;
            },
          };
        },
      };
      strictEqual(
        desiredSizeOrError(() => make(strategy)),
        4
      );
      strictEqual(reads, 1);
      strictEqual(conversions, 1);
      for (const [highWaterMark, expected] of [
        ['3', 3],
        [true, 1],
        [null, 0],
        [8, Math.min(8, capped)],
      ]) {
        strictEqual(
          desiredSizeOrError(() => make({ highWaterMark })),
          expected,
          String(highWaterMark)
        );
      }
    }
  },
};

// The conversion itself diverges (ledger #24): TypeScript applies the
// WHATWG writable conversion (NaN or negative is a RangeError; Infinity
// and fractions are kept; a bigint is a TypeError), the same for both
// classes; C++ converts to a uint64 (TypeError for non-finite or negative
// values; fractions truncate; a bigint converts) and narrows desiredSize
// to a 32-bit int.
export const highWaterMarkConversionDivergence = {
  test() {
    // [highWaterMark, TS ITS, TS FLS(5), C++ ITS, C++ FLS(5)]
    const cases = [
      ['abc', 'RangeError', 'RangeError', 'TypeError', 'TypeError'],
      [NaN, 'RangeError', 'RangeError', 'TypeError', 'TypeError'],
      [-1, 'RangeError', 'RangeError', 'TypeError', 'TypeError'],
      [-Infinity, 'RangeError', 'RangeError', 'TypeError', 'TypeError'],
      [Infinity, Infinity, 5, 'TypeError', 'TypeError'],
      [1.5, 1.5, 1.5, 1, 1],
      [3n, 'TypeError', 'TypeError', 3, 3],
      [2 ** 32 + 5, 2 ** 32 + 5, 5, 5, 5],
    ];
    for (const [highWaterMark, tsIts, tsFls, cppIts, cppFls] of cases) {
      deepStrictEqual(
        [
          desiredSizeOrError(
            () => new IdentityTransformStream({ highWaterMark })
          ),
          desiredSizeOrError(() => new FixedLengthStream(5, { highWaterMark })),
        ],
        usingTsImpl ? [tsIts, tsFls] : [cppIts, cppFls],
        String(highWaterMark)
      );
    }
  },
};
